// SPDX-License-Identifier: GPL-2.0-or-later
#include "dos_fs.h"

#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <ctime>
#include <filesystem>
#include <system_error>

#include "log.h"
#include "platform.h"

#ifndef O_BINARY
#define O_BINARY 0  // Windows: never translate line endings
#endif

namespace fg {

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Find `name` in host directory `dir` ignoring case. Returns the real name or "".
std::string match_in_dir(const std::string& dir, const std::string& name) {
    DIR* d = opendir(dir.c_str());
    if (!d) return "";
    std::string found;
    while (dirent* e = readdir(d)) {
        if (strcasecmp(e->d_name, name.c_str()) == 0) { found = e->d_name; break; }
    }
    closedir(d);
    return found;
}

uint16_t dos_errno(int e) {
    switch (e) {
    case ENOENT: return doserr::kFileNotFound;
    case ENOTDIR: return doserr::kPathNotFound;
    case EACCES: case EPERM: case EROFS: case EISDIR: return doserr::kAccessDenied;
    case EMFILE: case ENFILE: return doserr::kTooManyFiles;
    default: return doserr::kAccessDenied;
    }
}

void dos_datetime(time_t t, uint16_t& time, uint16_t& date) {
    struct tm tm;
    local_time(t, tm);
    time = static_cast<uint16_t>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    date = static_cast<uint16_t>(((std::max(tm.tm_year, 80) - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
}

// Name as DOS would show it, or "" if the host name is not a valid 8.3 name.
std::string to_83(const std::string& host_name) {
    if (host_name == "." || host_name == "..") return host_name;
    auto dot = host_name.find('.');
    std::string base = host_name.substr(0, dot);
    std::string ext = dot == std::string::npos ? "" : host_name.substr(dot + 1);
    if (base.empty() || base.size() > 8 || ext.size() > 3 || ext.find('.') != std::string::npos) return "";
    return upper(ext.empty() ? base : base + "." + ext);
}

}  // namespace

bool dos_wildcard_match(const std::string& pattern, const std::string& name) {
    auto expand = [](const std::string& s, std::string& b, std::string& e) {
        auto dot = s.find('.');
        b = s.substr(0, dot);
        e = dot == std::string::npos ? "" : s.substr(dot + 1);
    };
    auto field = [](const std::string& pat, const std::string& val, size_t width) {
        std::string p = pat, v = val;
        auto star = p.find('*');
        if (star != std::string::npos) p = p.substr(0, star) + std::string(width - std::min(width, star), '?');
        p.resize(width, ' ');
        v.resize(width, ' ');
        for (size_t i = 0; i < width; ++i)
            if (p[i] != '?' && std::toupper(static_cast<unsigned char>(p[i])) != std::toupper(static_cast<unsigned char>(v[i])))
                return false;
        return true;
    };
    std::string pb, pe, nb, ne;
    std::string pat = pattern == "*" ? "*.*" : pattern;
    expand(pat, pb, pe);
    expand(name, nb, ne);
    return field(pb, nb, 8) && field(pe, ne, 3);
}

void DosFs::map_drive(char letter, const std::string& host_dir) {
    int i = std::toupper(static_cast<unsigned char>(letter)) - 'A';
    drives_[i] = host_dir;
    cwd_[i].clear();
}

void DosFs::set_overlay(char letter, const std::string& host_dir) {
    overlays_[std::toupper(static_cast<unsigned char>(letter)) - 'A'] = host_dir;
}

bool DosFs::has_drive(char letter) const {
    int i = std::toupper(static_cast<unsigned char>(letter)) - 'A';
    return i >= 0 && i < 26 && !drives_[i].empty();
}

bool DosFs::set_current_drive(char letter) {
    if (!has_drive(letter)) return false;
    cur_drive_ = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
    return true;
}

std::string DosFs::cwd(char letter) const { return cwd_[std::toupper(static_cast<unsigned char>(letter)) - 'A']; }

bool DosFs::split(const std::string& dos_path, char& drive, std::vector<std::string>& parts) const {
    std::string p = dos_path;
    drive = cur_drive_;
    if (p.size() >= 2 && p[1] == ':') {
        drive = static_cast<char>(std::toupper(static_cast<unsigned char>(p[0])));
        p = p.substr(2);
    }
    if (!has_drive(drive)) return false;
    parts.clear();
    bool absolute = !p.empty() && (p[0] == '\\' || p[0] == '/');
    if (!absolute) {
        std::string c = cwd(drive);
        size_t s = 0;
        while (s < c.size()) {
            size_t e = c.find('\\', s);
            if (e == std::string::npos) e = c.size();
            if (e > s) parts.push_back(c.substr(s, e - s));
            s = e + 1;
        }
    }
    size_t s = 0;
    while (s <= p.size()) {
        size_t e = p.find_first_of("\\/", s);
        if (e == std::string::npos) e = p.size();
        std::string comp = p.substr(s, e - s);
        if (comp == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!comp.empty() && comp != ".") parts.push_back(comp);
        s = e + 1;
    }
    return true;
}

uint16_t DosFs::resolve_in(const std::string& root, const std::vector<std::string>& parts, bool must_exist,
                          std::string& host) const {
    host = root;
    for (size_t i = 0; i < parts.size(); ++i) {
        std::string real = match_in_dir(host, parts[i]);
        for (const auto& h : hidden_)
            if (strcasecmp(h.c_str(), parts[i].c_str()) == 0) real.clear();
        bool last = i + 1 == parts.size();
        if (real.empty()) {
            if (last && !must_exist) { host += "/" + upper(parts[i]); return 0; }
            return last ? doserr::kFileNotFound : doserr::kPathNotFound;
        }
        host += "/" + real;
    }
    return 0;
}

uint16_t DosFs::resolve(const std::string& dos_path, std::string& host) const {
    char drive;
    std::vector<std::string> parts;
    if (!split(dos_path, drive, parts)) return doserr::kPathNotFound;
    const std::string& overlay = overlays_[drive - 'A'];
    if (!overlay.empty() && resolve_in(overlay, parts, true, host) == 0) return 0;
    return resolve_in(drives_[drive - 'A'], parts, true, host);
}

uint16_t DosFs::resolve_write(const std::string& dos_path, bool copy_existing, std::string& host) {
    char drive;
    std::vector<std::string> parts;
    if (!split(dos_path, drive, parts)) return doserr::kPathNotFound;
    const std::string& base = drives_[drive - 'A'];
    const std::string& overlay = overlays_[drive - 'A'];
    if (overlay.empty()) return resolve_in(base, parts, false, host);
    if (parts.empty()) return doserr::kAccessDenied;
    // The overlay mirrors the base tree: make the directories on the way,
    // named as in the base (DOS directories must exist in one of the two).
    std::string dir = overlay, base_dir = base;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        std::string in_base = base_dir.empty() ? "" : match_in_dir(base_dir, parts[i]);
        base_dir = in_base.empty() ? "" : base_dir + "/" + in_base;
        std::string real = match_in_dir(dir, parts[i]);
        if (real.empty()) {
            if (in_base.empty()) return doserr::kPathNotFound;
            real = in_base;
            std::error_code ec;
            std::filesystem::create_directory(std::filesystem::path(dir + "/" + real), ec);
            if (ec) { FG_LOG("cannot create %s/%s: %s", dir.c_str(), real.c_str(), ec.message().c_str()); return doserr::kAccessDenied; }
        }
        dir += "/" + real;
    }
    const std::string& name = parts.back();
    if (std::string real = match_in_dir(dir, name); !real.empty()) { host = dir + "/" + real; return 0; }
    std::string in_base = base_dir.empty() ? "" : match_in_dir(base_dir, name);
    host = dir + "/" + (in_base.empty() ? upper(name) : in_base);
    if (copy_existing && !in_base.empty()) {
        std::error_code ec;
        std::filesystem::copy_file(std::filesystem::path(base_dir + "/" + in_base), std::filesystem::path(host), ec);
        if (ec) { FG_LOG("cannot copy %s to the overlay: %s", in_base.c_str(), ec.message().c_str()); return doserr::kAccessDenied; }
        // The copy keeps a read-only original's permissions (or attribute).
        std::filesystem::permissions(std::filesystem::path(host), std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::add, ec);
    }
    return 0;
}

uint16_t DosFs::chdir(const std::string& dos_path) {
    char drive;
    std::vector<std::string> parts;
    if (!split(dos_path, drive, parts)) return doserr::kPathNotFound;
    std::string host;
    if (resolve(dos_path, host)) return doserr::kPathNotFound;
    struct stat st;
    if (stat(host.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return doserr::kPathNotFound;
    std::string c;
    for (auto& p : parts) c += (c.empty() ? "" : "\\") + upper(p);
    cwd_[drive - 'A'] = c;
    return 0;
}

int DosFs::new_handle(int fd, const std::string& host) {
    for (size_t i = 5; i < handles_.size(); ++i)
        if (handles_[i].fd < 0) { handles_[i] = {fd, host}; return int(i); }
    if (handles_.size() >= 255) { ::close(fd); return -doserr::kTooManyFiles; }
    handles_.push_back({fd, host});
    return int(handles_.size() - 1);
}

bool DosFs::is_open(int h) const {
    return is_device(h) || (h >= 0 && size_t(h) < handles_.size() && handles_[h].fd >= 0);
}

std::string DosFs::host_path_of(int h) const { return is_open(h) && !is_device(h) ? handles_[h].host : ""; }

int DosFs::open(const std::string& dos_path, int mode) {
    std::string host;
    if (uint16_t e = resolve(dos_path, host)) return -e;
    int flags = (mode & 3) == 0 ? O_RDONLY : (mode & 3) == 1 ? O_WRONLY : O_RDWR;
    if (flags != O_RDONLY)
        if (uint16_t e = resolve_write(dos_path, true, host)) return -e;
    int fd = ::open(host.c_str(), flags | O_BINARY);
    if (fd < 0 && flags != O_RDONLY && errno == EACCES) return -doserr::kAccessDenied;
    if (fd < 0) return -dos_errno(errno);
    FG_TRACE("open %s -> %s (mode %d)", dos_path.c_str(), host.c_str(), mode);
    return new_handle(fd, host);
}

int DosFs::create(const std::string& dos_path) {
    std::string host;
    if (uint16_t e = resolve_write(dos_path, false, host)) return -e;
    int fd = ::open(host.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (fd < 0) return -dos_errno(errno);
    FG_TRACE("create %s -> %s", dos_path.c_str(), host.c_str());
    return new_handle(fd, host);
}

uint16_t DosFs::close(int h) {
    if (is_device(h)) return 0;
    if (!is_open(h)) return doserr::kInvalidHandle;
    ::close(handles_[h].fd);
    handles_[h] = {};
    return 0;
}

long DosFs::read(int h, void* buf, uint32_t n) {
    if (h == 0) return 0;  // no console input
    if (!is_open(h) || is_device(h)) return -doserr::kInvalidHandle;
    ssize_t r = ::read(handles_[h].fd, buf, n);
    return r < 0 ? -doserr::kAccessDenied : r;
}

long DosFs::write(int h, const void* buf, uint32_t n) {
    if (h == 1 || h == 2) return ::write(h, buf, n) < 0 ? 0 : long(n);
    if (h == 3 || h == 4) return long(n);
    if (!is_open(h)) return -doserr::kInvalidHandle;
    if (n == 0) {  // DOS: writing 0 bytes truncates at the current position
        off_t pos = lseek(handles_[h].fd, 0, SEEK_CUR);
        return ftruncate(handles_[h].fd, pos) == 0 ? 0 : -doserr::kAccessDenied;
    }
    ssize_t r = ::write(handles_[h].fd, buf, n);
    return r < 0 ? -doserr::kAccessDenied : r;
}

long DosFs::seek(int h, int32_t off, int whence) {
    if (is_device(h)) return 0;
    if (!is_open(h)) return -doserr::kInvalidHandle;
    off_t r = lseek(handles_[h].fd, off, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
    return r < 0 ? -0x19 : long(r);
}

// With an overlay the base file goes too if it can (otherwise it shows
// through again; the game only deletes files it wrote itself).
uint16_t DosFs::unlink(const std::string& dos_path) {
    std::string host;
    if (uint16_t e = resolve(dos_path, host)) return e;
    uint16_t e = ::unlink(host.c_str()) == 0 ? 0 : dos_errno(errno);
    char drive;
    std::vector<std::string> parts;
    if (!e && split(dos_path, drive, parts) && !overlays_[drive - 'A'].empty() &&
        resolve_in(drives_[drive - 'A'], parts, true, host) == 0)
        ::unlink(host.c_str());
    return e;
}

uint16_t DosFs::rename(const std::string& from, const std::string& to) {
    std::string src, dst;
    if (uint16_t e = resolve(from, src)) return e;
    if (uint16_t e = resolve_write(to, false, dst)) return e;
    std::error_code ec;
    std::filesystem::rename(std::filesystem::path(src), std::filesystem::path(dst), ec);
    if (ec) {  // a base file into the overlay: copy, then drop the original if allowed
        std::filesystem::copy_file(std::filesystem::path(src), std::filesystem::path(dst),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return doserr::kAccessDenied;
        std::filesystem::remove(std::filesystem::path(src), ec);
    }
    return 0;
}

uint16_t DosFs::find_first(uint32_t dta, const std::string& pattern, uint8_t attr, FindResult& out) {
    std::string dir_part, name_part = pattern;
    auto slash = pattern.find_last_of("\\/:");
    if (slash != std::string::npos) {
        dir_part = pattern.substr(0, slash + 1);
        name_part = pattern.substr(slash + 1);
    }
    std::string dir_dos = dir_part.empty() ? "." : dir_part;
    if (!dir_dos.empty() && dir_dos.back() == ':') dir_dos += ".";
    char drive;
    std::vector<std::string> parts;
    if (!split(dir_dos, drive, parts)) return doserr::kPathNotFound;
    std::vector<std::string> dirs;  // overlay first: its entries win
    std::string host;
    if (!overlays_[drive - 'A'].empty() && resolve_in(overlays_[drive - 'A'], parts, true, host) == 0)
        dirs.push_back(host);
    if (resolve_in(drives_[drive - 'A'], parts, true, host) == 0) dirs.push_back(host);
    if (dirs.empty()) return doserr::kPathNotFound;

    Search s;
    for (const auto& dir : dirs) {
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        while (dirent* e = readdir(d)) {
            std::string n83 = to_83(e->d_name);
            if (n83.empty() || !dos_wildcard_match(name_part, n83)) continue;
            if (std::any_of(s.results.begin(), s.results.end(), [&](auto& r) { return r.name == n83; }))
                continue;
            struct stat st;
            if (stat((dir + "/" + e->d_name).c_str(), &st) != 0) continue;
            uint8_t a = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
            if ((a & 0x10) && !(attr & 0x10)) continue;  // directories only when asked for
            FindResult r{n83, a, 0, 0, static_cast<uint32_t>(st.st_size)};
            dos_datetime(st.st_mtime, r.time, r.date);
            s.results.push_back(r);
        }
        closedir(d);
    }
    std::sort(s.results.begin(), s.results.end(), [](auto& a, auto& b) { return a.name < b.name; });
    searches_[dta] = std::move(s);
    return find_next(dta, out) == 0 ? 0 : doserr::kFileNotFound;
}

uint16_t DosFs::find_next(uint32_t dta, FindResult& out) {
    auto it = searches_.find(dta);
    if (it == searches_.end() || it->second.next >= it->second.results.size()) return doserr::kNoMoreFiles;
    out = it->second.results[it->second.next++];
    return 0;
}

}  // namespace fg
