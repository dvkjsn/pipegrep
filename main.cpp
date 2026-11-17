#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <fstream>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cctype>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "BoundedBuffer.h"

// record passed through buffers 3 and 4: one line of one file
struct LineRecord {
    std::string file;  // relative path beginning with ./
    size_t num;        // one-based line number
    std::string text;  // line contents without the terminating newline
};

// set when any file or directory error occurs; makes the program exit nonzero
static std::atomic<bool> had_error(false);

// command-line filters; a value of -1 means the filter is disabled
struct Filters {
    long long filesize;
    long long uid;
    long long gid;
};

// parses a base-10 integer; rejects empty input, trailing text, and out-of-range values
static bool parse_ll(const char* s, long long& out) {
    if (!isdigit((unsigned char)s[0]) &&
        !((s[0] == '-' || s[0] == '+') && isdigit((unsigned char)s[1]))) return false;
    errno = 0;
    char* end;
    long long v = strtoll(s, &end, 10);
    if (errno == ERANGE || *end != '\0') return false;
    out = v;
    return true;
}

// stage 1: enumerates the current directory and sends each entry as "./name" to buffer 1.
// closes buffer 1 when finished.
static void stage1(BoundedBuffer<std::string>& out) {
    DIR* d = opendir(".");
    if (!d) {
        std::cerr << "pipegrep: cannot open current directory: " << strerror(errno) << "\n";
        had_error = true;
    } else {
        while (dirent* e = readdir(d)) {
            std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            out.push("./" + name);
        }
        closedir(d);
    }
    out.close();
}

// stage 2: reads paths from buffer 1 and forwards to buffer 2 only regular files
// that satisfy every enabled filter. lstat is used so symlinks are not followed.
// closes buffer 2 when buffer 1 is exhausted.
static void stage2(BoundedBuffer<std::string>& in, BoundedBuffer<std::string>& out, const Filters& f) {
    std::string path;
    while (in.pop(path)) {
        struct stat st;
        if (lstat(path.c_str(), &st) != 0) {
            std::cerr << "pipegrep: cannot inspect " << path << ": " << strerror(errno) << "\n";
            had_error = true;
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;
        if (f.filesize >= 0 && (long long)st.st_size <= f.filesize) continue;
        if (f.uid >= 0 && (long long)st.st_uid != f.uid) continue;
        if (f.gid >= 0 && (long long)st.st_gid != f.gid) continue;
        out.push(path);
    }
    out.close();
}

// stage 3: reads paths from buffer 2, reads each file line by line, and sends one
// LineRecord per line to buffer 3. a final line without a newline is still sent.
// unreadable files are reported and skipped. closes buffer 3 when buffer 2 is exhausted.
static void stage3(BoundedBuffer<std::string>& in, BoundedBuffer<LineRecord>& out) {
    std::string path;
    while (in.pop(path)) {
        std::ifstream file(path);
        if (!file) {
            std::cerr << "pipegrep: cannot open " << path << ": " << strerror(errno) << "\n";
            had_error = true;
            continue;
        }
        std::string line;
        size_t num = 0;
        while (std::getline(file, line)) {
            num++;
            out.push(LineRecord{path, num, line});
        }
        if (file.bad()) {
            std::cerr << "pipegrep: read error in " << path << "\n";
            had_error = true;
        }
    }
    out.close();
}

// stage 4: reads records from buffer 3 and forwards to buffer 4 only those whose text
// contains the search string as a case-sensitive literal substring.
// closes buffer 4 when buffer 3 is exhausted.
static void stage4(BoundedBuffer<LineRecord>& in, BoundedBuffer<LineRecord>& out, const std::string& needle) {
    LineRecord rec;
    while (in.pop(rec)) {
        if (rec.text.find(needle) != std::string::npos) out.push(rec);
    }
    out.close();
}

// stage 5: reads matching records from buffer 4, prints each as "file(line): text",
// then prints the final match count. this is the only stage that writes to stdout.
static void stage5(BoundedBuffer<LineRecord>& in) {
    LineRecord rec;
    size_t count = 0;
    while (in.pop(rec)) {
        std::cout << rec.file << "(" << rec.num << "): " << rec.text << "\n";
        count++;
    }
    std::cout << "***** You found " << count << " matches *****" << std::endl;
}

// prints usage to stderr and returns the failure exit status
static int usage() {
    std::cerr << "usage: ./pipegrep buffsize filesize uid gid \"string\"\n"
              << "  buffsize >= 1; filesize, uid, gid >= 0 or -1 to disable; string nonempty\n";
    return 1;
}

// validates arguments, creates the four buffers, starts the five stage threads,
// and joins them. returns 0 on success, nonzero on argument or file errors.
int main(int argc, char* argv[]) {
    if (argc != 6) return usage();

    long long buffsize, filesize, uid, gid;
    if (!parse_ll(argv[1], buffsize) || buffsize < 1) return usage();
    if (!parse_ll(argv[2], filesize) || filesize < -1) return usage();
    if (!parse_ll(argv[3], uid) || uid < -1 ||
        uid > (long long)std::numeric_limits<uid_t>::max()) return usage();
    if (!parse_ll(argv[4], gid) || gid < -1 ||
        gid > (long long)std::numeric_limits<gid_t>::max()) return usage();
    if (argv[5][0] == '\0') return usage();

    std::string needle = argv[5];
    Filters filters{filesize, uid, gid};

    // buffer 1: paths from stage 1 to 2    buffer 2: paths from stage 2 to 3
    // buffer 3: lines from stage 3 to 4    buffer 4: matches from stage 4 to 5
    BoundedBuffer<std::string> buf1(buffsize);
    BoundedBuffer<std::string> buf2(buffsize);
    BoundedBuffer<LineRecord> buf3(buffsize);
    BoundedBuffer<LineRecord> buf4(buffsize);

    // the main thread only creates and joins workers; it performs no stage work
    std::vector<std::thread> workers;
    try {
        workers.emplace_back(stage1, std::ref(buf1));
        workers.emplace_back(stage2, std::ref(buf1), std::ref(buf2), std::cref(filters));
        workers.emplace_back(stage3, std::ref(buf2), std::ref(buf3));
        workers.emplace_back(stage4, std::ref(buf3), std::ref(buf4), std::cref(needle));
        workers.emplace_back(stage5, std::ref(buf4));
    } catch (const std::exception& e) {
        std::cerr << "pipegrep: cannot create worker thread: " << e.what() << "\n";
        std::exit(1);
    }

    for (auto& t : workers) t.join();
    return had_error ? 1 : 0;
}