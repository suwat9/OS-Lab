// Lab 1: Display basic system information (Linux)
//   1. CPU name
//   2. Memory
//   3. OS name and version
//   4. Disk storage
//   5. Running processes
//
// Build: g++ -std=c++17 -Wall -Wextra -O2 -o lab1 lab1.cpp

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------- utilities

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Find "key<sep> value" in a text file and return the trimmed value.
// Surrounding double quotes (used by /etc/os-release) are removed.
std::string valueAfter(const std::string& text, const std::string& key, char sep) {
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        const auto pos = line.find(sep);
        if (pos == std::string::npos) continue;
        if (trim(line.substr(0, pos)) != key) continue;

        std::string value = trim(line.substr(pos + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        return value;
    }
    return "";
}

std::string valueAfterColon(const std::string& text, const std::string& key) {
    return valueAfter(text, key, ':');
}

std::string valueAfterEquals(const std::string& text, const std::string& key) {
    return valueAfter(text, key, '=');
}

unsigned long long parseULL(const std::string& s) {
    std::istringstream iss(s);
    unsigned long long value = 0;
    iss >> value;
    return value;
}

std::string humanBytes(unsigned long long bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 2)
        << value << ' ' << units[unit];
    return out.str();
}

std::string percent(unsigned long long part, unsigned long long total) {
    if (total == 0) return "0.0%";
    std::ostringstream out;
    out << std::fixed << std::setprecision(1)
        << (100.0 * static_cast<double>(part) / static_cast<double>(total)) << '%';
    return out.str();
}

void section(const std::string& title) {
    std::cout << '\n' << std::string(64, '=') << '\n'
              << ' ' << title << '\n'
              << std::string(64, '=') << '\n';
}

void printField(const std::string& label, const std::string& value) {
    std::cout << std::left << std::setw(16) << label << ": " << value << '\n';
}

// --------------------------------------------------------------------- CPU

void showCPU() {
    section("1. CPU");

    const std::string cpuinfo = readFile("/proc/cpuinfo");

    std::string model = valueAfterColon(cpuinfo, "model name");
    if (model.empty()) model = valueAfterColon(cpuinfo, "Hardware");  // ARM fallback
    if (model.empty()) model = "Unknown";

    int logicalCPUs = 0;
    {
        std::istringstream iss(cpuinfo);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.rfind("processor", 0) == 0 && line.find(':') != std::string::npos)
                ++logicalCPUs;
        }
    }
    if (logicalCPUs == 0) {
        const long online = sysconf(_SC_NPROCESSORS_ONLN);
        logicalCPUs = online > 0 ? static_cast<int>(online) : 0;
    }

    printField("Model", model);
    printField("Logical CPUs", std::to_string(logicalCPUs));
}

// ------------------------------------------------------------------ memory

void showMemory() {
    section("2. Memory");

    const std::string meminfo = readFile("/proc/meminfo");

    auto memBytes = [&meminfo](const std::string& key) -> unsigned long long {
        std::istringstream iss(valueAfterColon(meminfo, key));
        unsigned long long value = 0;
        std::string unit;
        iss >> value >> unit;
        return unit == "kB" ? value * 1024ULL : value;
    };

    const unsigned long long total = memBytes("MemTotal");
    unsigned long long available = memBytes("MemAvailable");
    if (available == 0) available = memBytes("MemFree");
    const unsigned long long used = total > available ? total - available : 0;

    printField("Total", humanBytes(total));
    printField("Used", humanBytes(used) + "  (" + percent(used, total) + ")");
    printField("Available", humanBytes(available));

    const unsigned long long swapTotal = memBytes("SwapTotal");
    if (swapTotal > 0) {
        const unsigned long long swapFree = memBytes("SwapFree");
        printField("Swap total", humanBytes(swapTotal));
        printField("Swap used", humanBytes(swapTotal - swapFree));
    }
}

// ---------------------------------------------------------------------- OS

void showOS() {
    section("3. Operating System");

    const std::string release = readFile("/etc/os-release");

    std::string name = valueAfterEquals(release, "PRETTY_NAME");
    if (name.empty()) name = valueAfterEquals(release, "NAME");
    if (name.empty()) name = "Unknown";

    std::string version = valueAfterEquals(release, "VERSION");
    if (version.empty()) version = valueAfterEquals(release, "VERSION_ID");
    if (version.empty()) version = "Unknown";

    printField("OS", name);
    printField("Version", version);

    struct utsname uts {};
    if (uname(&uts) == 0) {
        printField("Kernel", std::string(uts.sysname) + " " + uts.release);
        printField("Architecture", uts.machine);
        printField("Hostname", uts.nodename);
    }
}

// -------------------------------------------------------------------- disk

struct MountInfo {
    std::string device;
    std::string type;
    std::string point;
};

std::vector<MountInfo> readDeviceMounts() {
    // One entry per device: bind mounts are collapsed and file bind mounts
    // (e.g. /etc/hosts) are skipped, so only real storage is shown.
    std::map<std::string, MountInfo> byDevice;

    std::ifstream in("/proc/mounts");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        MountInfo m;
        std::string options, dump, pass;
        if (!(iss >> m.device >> m.point >> m.type >> options >> dump >> pass))
            continue;
        if (m.device.rfind("/dev/", 0) != 0)
            continue;

        std::error_code ec;
        if (fs::is_regular_file(m.point, ec))
            continue;

        auto it = byDevice.find(m.device);
        if (it == byDevice.end() || m.point.size() < it->second.point.size())
            byDevice[m.device] = m;
    }

    std::vector<MountInfo> mounts;
    mounts.reserve(byDevice.size());
    for (const auto& entry : byDevice)
        mounts.push_back(entry.second);
    return mounts;
}

void printUsage(const std::string& device, const std::string& type,
                const std::string& point) {
    struct statvfs vfs {};
    if (statvfs(point.c_str(), &vfs) != 0)
        return;

    const unsigned long long blockSize = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    const unsigned long long total = static_cast<unsigned long long>(vfs.f_blocks) * blockSize;
    const unsigned long long free = static_cast<unsigned long long>(vfs.f_bfree) * blockSize;
    const unsigned long long available = static_cast<unsigned long long>(vfs.f_bavail) * blockSize;
    const unsigned long long used = total > free ? total - free : 0;

    std::cout << std::left
              << std::setw(22) << device
              << std::setw(12) << type
              << std::setw(22) << point
              << std::right
              << std::setw(11) << humanBytes(total)
              << std::setw(11) << humanBytes(used)
              << std::setw(11) << humanBytes(available)
              << std::setw(8) << percent(used, total)
              << '\n';
}

void showDisk() {
    section("4. Disk Storage");

    std::cout << "Root filesystem (/):\n";
    struct statvfs vfs {};
    if (statvfs("/", &vfs) == 0) {
        const unsigned long long blockSize = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
        const unsigned long long total = static_cast<unsigned long long>(vfs.f_blocks) * blockSize;
        const unsigned long long free = static_cast<unsigned long long>(vfs.f_bfree) * blockSize;
        const unsigned long long available = static_cast<unsigned long long>(vfs.f_bavail) * blockSize;
        const unsigned long long used = total > free ? total - free : 0;

        printField("Total", humanBytes(total));
        printField("Used", humanBytes(used) + "  (" + percent(used, total) + ")");
        printField("Available", humanBytes(available));
    } else {
        std::cout << "  Unable to read filesystem statistics.\n";
    }

    const std::vector<MountInfo> mounts = readDeviceMounts();
    if (mounts.empty())
        return;

    std::cout << "\nMounted storage devices:\n"
              << std::left
              << std::setw(22) << "Filesystem"
              << std::setw(12) << "Type"
              << std::setw(22) << "Mounted on"
              << std::right
              << std::setw(11) << "Size"
              << std::setw(11) << "Used"
              << std::setw(11) << "Avail"
              << std::setw(8) << "Use%"
              << '\n'
              << std::string(97, '-') << '\n';

    for (const MountInfo& m : mounts)
        printUsage(m.device, m.type, m.point);
}

// --------------------------------------------------------------- processes

struct Process {
    int pid = 0;
    int ppid = 0;
    std::string state;
    unsigned long long rssBytes = 0;
    std::string name;
};

std::vector<Process> readProcesses() {
    std::vector<Process> processes;

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/proc", ec)) {
        if (ec) break;

        const std::string pidText = entry.path().filename().string();
        if (pidText.empty() ||
            !std::all_of(pidText.begin(), pidText.end(),
                         [](unsigned char c) { return std::isdigit(c) != 0; }))
            continue;

        const std::string status = readFile((entry.path() / "status").string());
        if (status.empty()) continue;  // process exited while we were reading

        Process p;
        p.pid = std::stoi(pidText);
        p.name = valueAfterColon(status, "Name");
        if (p.name.empty()) continue;

        p.state = valueAfterColon(status, "State");
        p.ppid = static_cast<int>(parseULL(valueAfterColon(status, "PPid")));
        p.rssBytes = parseULL(valueAfterColon(status, "VmRSS")) * 1024ULL;
        processes.push_back(p);
    }

    std::sort(processes.begin(), processes.end(),
              [](const Process& a, const Process& b) { return a.pid < b.pid; });
    return processes;
}

void showProcesses() {
    section("5. Running Processes");

    const std::vector<Process> processes = readProcesses();

    std::cout << std::left
              << std::setw(8) << "PID"
              << std::setw(8) << "PPID"
              << std::setw(18) << "STATE"
              << std::right << std::setw(12) << "MEMORY" << "  "
              << std::left << "NAME" << '\n'
              << std::string(78, '-') << '\n';

    for (const Process& p : processes) {
        std::cout << std::left
                  << std::setw(8) << p.pid
                  << std::setw(8) << p.ppid
                  << std::setw(18) << p.state
                  << std::right << std::setw(12) << humanBytes(p.rssBytes) << "  "
                  << std::left << p.name << '\n';
    }

    std::cout << "\nTotal processes: " << processes.size() << '\n';
}

}  // namespace

int main() {
    std::cout << "System Information (Lab 1)\n";

    showCPU();
    showMemory();
    showOS();
    showDisk();
    showProcesses();

    std::cout << '\n';
    return 0;
}
