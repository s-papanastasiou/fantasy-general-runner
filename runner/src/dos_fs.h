// SPDX-License-Identifier: GPL-2.0-or-later
// DOS file system emulation: drive letters mapped to host directories,
// case-insensitive path resolution, DOS file handles, find first/next.
// A drive can have a writable overlay directory (like DOSBox's
// "mount -t overlay"): files there hide the base directory's, and every
// file created or opened for writing lands there.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fg {

namespace doserr {
constexpr uint16_t kFileNotFound = 2, kPathNotFound = 3, kTooManyFiles = 4, kAccessDenied = 5,
                   kInvalidHandle = 6, kInvalidAccess = 0x0C, kInvalidDrive = 0x0F, kNoMoreFiles = 0x12;
}

struct FindResult {
    std::string name;  // 8.3 upper case as DOS shows it
    uint8_t attr;
    uint16_t time, date;
    uint32_t size;
};

class DosFs {
public:
    void map_drive(char letter, const std::string& host_dir);
    bool has_drive(char letter) const;
    void set_overlay(char letter, const std::string& host_dir);
    // Pretend files with this name (any directory, case-insensitive) do not exist.
    void hide(const std::string& name) { hidden_.push_back(name); }

    char current_drive() const { return cur_drive_; }
    bool set_current_drive(char letter);
    std::string cwd(char letter) const;            // e.g. "EXE" (no leading backslash)
    uint16_t chdir(const std::string& dos_path);   // 0 or DOS error

    // Resolve a DOS path to a host path for reading (the overlay's file if
    // there is one). Returns 0 or a DOS error.
    uint16_t resolve(const std::string& dos_path, std::string& host) const;
    // The host path to create or write `dos_path` at: in the overlay if the
    // drive has one (making its directories; `copy_existing` copies the base
    // file there first), else in the base directory. Returns 0 or a DOS error.
    uint16_t resolve_write(const std::string& dos_path, bool copy_existing, std::string& host);

    // File handles. Results: handle (>= 0) or -(DOS error).
    int open(const std::string& dos_path, int mode);   // mode: 0 read, 1 write, 2 read/write
    int create(const std::string& dos_path);
    uint16_t close(int handle);
    long read(int handle, void* buf, uint32_t n);      // -(DOS error) on failure
    long write(int handle, const void* buf, uint32_t n);
    long seek(int handle, int32_t off, int whence);
    uint16_t unlink(const std::string& dos_path);
    uint16_t rename(const std::string& from, const std::string& to);
    bool is_open(int handle) const;
    bool is_device(int handle) const { return handle >= 0 && handle <= 4; }
    std::string host_path_of(int handle) const;

    // Find first/next, keyed by the DTA's linear address.
    uint16_t find_first(uint32_t dta, const std::string& pattern, uint8_t attr, FindResult& out);
    uint16_t find_next(uint32_t dta, FindResult& out);

private:
    struct Handle { int fd = -1; std::string host; };
    struct Search { std::vector<FindResult> results; size_t next = 0; };

    std::string drives_[26];
    std::string overlays_[26];
    std::string cwd_[26];
    char cur_drive_ = 'C';
    std::vector<Handle> handles_ = std::vector<Handle>(5);  // 0-4 reserved for devices
    std::map<uint32_t, Search> searches_;
    std::vector<std::string> hidden_;

    int new_handle(int fd, const std::string& host);
    // Walk `parts` below host directory `root`, matching names without case.
    uint16_t resolve_in(const std::string& root, const std::vector<std::string>& parts, bool must_exist,
                        std::string& host) const;
    // Split a DOS path into drive + absolute component list.
    bool split(const std::string& dos_path, char& drive, std::vector<std::string>& parts) const;
};

// 8.3 wildcard match (DOS semantics: '*' fills the rest of name or extension).
bool dos_wildcard_match(const std::string& pattern, const std::string& name);

}  // namespace fg
