#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <mntent.h>
#include <unistd.h>
#include <regex.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/types.h>

// Constants for terminal and display
#define TERM_FALLBACK_WIDTH 80
#define VERSION "1.0.4"
#define COLOR_BUFFER_SIZE 32
#define MAX_UNITS 5
#define BYTES_PER_KB 1024.0
#define MAX_UNIT_INDEX 4
#define PERCENTAGE_MULTIPLIER 100.0
#define USAGE_PERCENT_DIVISOR 100.0

// Constants for device path lengths
#define DEV_SD_PREFIX_LEN 7
#define DEV_NVME_PREFIX_LEN 9
#define DEV_HD_PREFIX_LEN 7
#define NETWORK_PATH_PREFIX_LEN 2
#define FUSE_PREFIX_LEN 5

// Constants for file paths and buffers
#define MAX_PATH_LENGTH 1024
#define MAX_GVFS_PATH_LENGTH 256
#define MAX_SIZE_STR_LENGTH 64
#define MAX_PERCENT_TEXT_LENGTH 16
#define MAX_TEMP_BUFFER_LENGTH 128
#define MAX_BAR_BUFFER_MULTIPLIER 64
#define BARLINE_BUFFER_EXTRA 16

// Constants for terminal width calculations
#define TERMINAL_WIDTH_PERCENTAGE 4
#define TERMINAL_WIDTH_DIVISOR 5
#define MAX_BOX_WIDTH 120
#define MIN_BOX_WIDTH 40
#define FRAME_PADDING 4
#define BRACKET_PADDING 2
#define MIN_BAR_LENGTH 10

// Constants for color values
#define MAX_COLOR_VALUE 255
#define COLOR_RATIO_MULTIPLIER 2
#define COLOR_RATIO_HALF 0.5f
#define GRAY_BACKGROUND_R 64
#define GRAY_BACKGROUND_G 64
#define GRAY_BACKGROUND_B 64
#define GRAY_FOREGROUND_R 160
#define GRAY_FOREGROUND_G 160
#define GRAY_FOREGROUND_B 160
#define BLUE_TEXT_R 0
#define BLUE_TEXT_G 0
#define BLUE_TEXT_B 255

// Constants for string formatting
#define PERCENT_FORMAT "%.1f%%"
#define COLOR_FORMAT "\033[38;2;%d;%d;%dm"
#define BACKGROUND_COLOR_FORMAT "\033[48;2;%d;%d;%dm"
#define BLUE_TEXT_FORMAT "\033[38;2;%d;%d;%dm"
#define RESET_FORMAT "\033[0m"
#define BOLD_YELLOW_FORMAT "\033[1;33m"
#define RESET_COLOR_FORMAT "\033[0m"

// Constants for file system types
#define MOUNT_TABLE_PATH "/proc/mounts"
#define GVFS_BASE_PATH "/run/user/%d/gvfs"

// Maximum number of drives to handle
#define MAX_DRIVES 100

// Constants for file system types to skip
const char *skip_filesystems[] = {
    "proc", "sysfs", "devpts", "tmpfs", "devtmpfs", "securityfs",
    "cgroup", "cgroup2", "pstore", "efivarfs", "autofs", "debugfs",
    "tracefs", "configfs", "fusectl", "fuse.gvfsd-fuse", "binfmt_misc",
    "fuse.portal"};

// Structure to hold drive information
typedef struct
{
    char mount_point[MAX_PATH_LENGTH];
    char filesystem[MAX_SIZE_STR_LENGTH];
    char device[MAX_PATH_LENGTH];
    char uuid[128];
    char label[128];
    char total_str[MAX_SIZE_STR_LENGTH];
    char used_str[MAX_SIZE_STR_LENGTH];
    char available_str[MAX_SIZE_STR_LENGTH];
    unsigned long long total_bytes;
    unsigned long long used_bytes;
    unsigned long long available_bytes;
    double usage_percent;
    const char *drive_type;
    char *progress_bar;
    bool is_cloud_storage;
    char cloud_service_name[MAX_SIZE_STR_LENGTH];
    char mount_options[MAX_TEMP_BUFFER_LENGTH];
    unsigned long long total_inodes;
    unsigned long long used_inodes;
    double inode_usage;
} drive_info_t;

char colorbuf[COLOR_BUFFER_SIZE]; // For bar colors

// Function to compare drives by total capacity (descending order)
int compare_drives_by_capacity(const void *a, const void *b)
{
    const drive_info_t *drive_a = (const drive_info_t *)a;
    const drive_info_t *drive_b = (const drive_info_t *)b;

    if (drive_b->total_bytes > drive_a->total_bytes)
        return 1;
    if (drive_b->total_bytes < drive_a->total_bytes)
        return -1;
    return 0;
}

// Function to display help text
void show_help(const char *program_name)
{
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("\n");
    printf("Display information about available drives and their storage space.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help     Show this help message\n");
    printf("  -v, --version  Show program version\n");
    printf("\n");
    printf("This program is licensed under the MIT License.\n");
    printf("https://github.com/lennart1978/drinfo\n");
}

// Function to display version
void show_version()
{
    printf("drinfo Version %s\n", VERSION);
}

// Function to get terminal width
int get_terminal_width()
{
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0)
    {
        return w.ws_col;
    }
    return TERM_FALLBACK_WIDTH; // Fallback-Value
}

// Function to format bytes into human-readable sizes
void format_bytes(unsigned long long bytes, char *buffer, size_t buffer_size)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double size = bytes;

    while (size >= BYTES_PER_KB && unit_index < MAX_UNIT_INDEX)
    {
        size /= BYTES_PER_KB;
        unit_index++;
    }

    if (unit_index == 0)
    {
        snprintf(buffer, buffer_size, "%.0f %s", size, units[unit_index]);
    }
    else
    {
        snprintf(buffer, buffer_size, "%.2f %s", size, units[unit_index]);
    }
}

// Function to calculate usage percentage
double calculate_usage_percent(unsigned long long total, unsigned long long available)
{
    if (total == 0)
        return 0.0;
    unsigned long long used = total - available;
    return ((double)used / total) * PERCENTAGE_MULTIPLIER;
}

bool is_physical_device(const char *fsname)
{
    // Check for /dev/sd*, /dev/nvme*, /dev/hd*
    return (strncmp(fsname, "/dev/sd", DEV_SD_PREFIX_LEN) == 0 ||
            strncmp(fsname, "/dev/nvme", DEV_NVME_PREFIX_LEN) == 0 ||
            strncmp(fsname, "/dev/hd", DEV_HD_PREFIX_LEN) == 0);
}

bool is_network_device(const char *fsname)
{
    // Check for network file systems
    return (strncmp(fsname, "//", NETWORK_PATH_PREFIX_LEN) == 0 ||   // SMB/CIFS shares
            strncmp(fsname, "\\\\", NETWORK_PATH_PREFIX_LEN) == 0 || // Windows network paths
            strstr(fsname, ":") != NULL);                            // NFS and other network protocols
}

bool is_network_filesystem(const char *fstype)
{
    // Check for network file system types
    return (strcmp(fstype, "nfs") == 0 ||
            strcmp(fstype, "nfs4") == 0 ||
            strcmp(fstype, "cifs") == 0 ||
            strcmp(fstype, "smb") == 0 ||
            strcmp(fstype, "smb3") == 0 ||
            strcmp(fstype, "fuse.sshfs") == 0 ||
            strcmp(fstype, "fuse.rclone") == 0 ||
            strcmp(fstype, "fuse.gvfsd-fuse") == 0 ||        // GVFS for cloud storage
            strncmp(fstype, "fuse.", FUSE_PREFIX_LEN) == 0); // Other FUSE-based network file systems
}

bool is_appimage_or_temp(const char *fsname, const char *mountpoint)
{
    // Filter out AppImages and temporary mounts
    return (strstr(fsname, ".AppImage") != NULL ||
            strstr(mountpoint, "/tmp/.mount_") != NULL ||
            strstr(mountpoint, "/tmp/") != NULL);
}

// Helper for true-color gradient (red-yellow-green)
void get_bar_color(int idx, int max, char *buffer, size_t size)
{
    // New gradient: 0% = green (0,255,0), 50% = yellow (255,255,0), 100% = red (255,0,0)
    float ratio = (float)idx / (float)(max - 1);
    int r, g, b;
    if (ratio < COLOR_RATIO_HALF)
    {
        // Green to Yellow
        r = (int)(ratio * COLOR_RATIO_MULTIPLIER * MAX_COLOR_VALUE);
        g = MAX_COLOR_VALUE;
        b = 0;
    }
    else
    {
        // Yellow to Red
        r = MAX_COLOR_VALUE;
        g = (int)((1.0f - (ratio - COLOR_RATIO_HALF) * COLOR_RATIO_MULTIPLIER) * MAX_COLOR_VALUE);
        b = 0;
    }
    snprintf(buffer, size, COLOR_FORMAT, r, g, b);
}

// Helper: visible length of a string without ANSI sequences
int visible_length(const char *s)
{
    int len = 0;
    int in_escape = 0;
    for (; *s; ++s)
    {
        if (*s == '\033')
            in_escape = 1;
        else if (in_escape && *s == 'm')
            in_escape = 0;
        else if (!in_escape)
            len++;
    }
    return len;
}

// Adjusted: calculate max visible line length for block
int max_visible_line_length(const char *lines[], int n)
{
    int max = 0;
    for (int i = 0; i < n; ++i)
    {
        int len = visible_length(lines[i]);
        if (len > max)
            max = len;
    }
    return max;
}

// Function to check if a directory contains cloud storage
bool is_cloud_storage_directory(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir)
        return false;

    struct dirent *entry;
    bool has_cloud_storage = false;

    while ((entry = readdir(dir)) != NULL)
    {
        // Use stat if d_type is not available
        struct stat st;
        char full_path[MAX_PATH_LENGTH];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode) &&
            (strstr(entry->d_name, "google-drive") != NULL ||
             strstr(entry->d_name, "dropbox") != NULL ||
             strstr(entry->d_name, "onedrive") != NULL ||
             strstr(entry->d_name, "mega") != NULL))
        {
            has_cloud_storage = true;
            break;
        }
    }

    closedir(dir);
    return has_cloud_storage;
}

// Function to fill drive_info_t structure with common data
void fill_drive_info(drive_info_t *drive,
                     const char *mount_point,
                     const char *filesystem,
                     const char *device,
                     const char *uuid,
                     const char *label,
                     const char *total_str,
                     const char *used_str,
                     const char *available_str,
                     unsigned long long total_bytes,
                     unsigned long long used_bytes,
                     unsigned long long available_bytes,
                     double usage_percent,
                     const char *drive_type,
                     char *progress_bar,
                     bool is_cloud_storage,
                     const char *cloud_service_name,
                     const char *mount_options,
                     unsigned long long total_inodes,
                     unsigned long long used_inodes,
                     double inode_usage)
{
    strncpy(drive->mount_point, mount_point, sizeof(drive->mount_point) - 1);
    drive->mount_point[sizeof(drive->mount_point) - 1] = '\0';
    strncpy(drive->filesystem, filesystem, sizeof(drive->filesystem) - 1);
    drive->filesystem[sizeof(drive->filesystem) - 1] = '\0';
    strncpy(drive->device, device, sizeof(drive->device) - 1);
    drive->device[sizeof(drive->device) - 1] = '\0';
    if (uuid)
    {
        strncpy(drive->uuid, uuid, sizeof(drive->uuid) - 1);
        drive->uuid[sizeof(drive->uuid) - 1] = '\0';
    }
    else
    {
        drive->uuid[0] = '\0';
    }
    if (label)
    {
        strncpy(drive->label, label, sizeof(drive->label) - 1);
        drive->label[sizeof(drive->label) - 1] = '\0';
    }
    else
    {
        drive->label[0] = '\0';
    }
    snprintf(drive->total_str, sizeof(drive->total_str), "%s", total_str);
    snprintf(drive->used_str, sizeof(drive->used_str), "%s", used_str);
    snprintf(drive->available_str, sizeof(drive->available_str), "%s", available_str);
    drive->total_bytes = total_bytes;
    drive->used_bytes = used_bytes;
    drive->available_bytes = available_bytes;
    drive->usage_percent = usage_percent;
    drive->drive_type = drive_type;
    drive->progress_bar = progress_bar;
    drive->is_cloud_storage = is_cloud_storage;
    if (cloud_service_name)
    {
        strncpy(drive->cloud_service_name, cloud_service_name, sizeof(drive->cloud_service_name) - 1);
        drive->cloud_service_name[sizeof(drive->cloud_service_name) - 1] = '\0';
    }
    else
    {
        drive->cloud_service_name[0] = '\0';
    }
    if (mount_options)
    {
        strncpy(drive->mount_options, mount_options, sizeof(drive->mount_options) - 1);
        drive->mount_options[sizeof(drive->mount_options) - 1] = '\0';
    }
    else
    {
        drive->mount_options[0] = '\0';
    }
    drive->total_inodes = total_inodes;
    drive->used_inodes = used_inodes;
    drive->inode_usage = inode_usage;
}

// Function to get cloud storage info from GVFS
void get_cloud_storage_info(const char *gvfs_path, drive_info_t *drives, int *drive_count)
{
    DIR *dir = opendir(gvfs_path);
    if (!dir)
        return;

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL && *drive_count < MAX_DRIVES)
    {
        // Use stat if d_type is not available
        struct stat st;
        char full_path[MAX_PATH_LENGTH];
        snprintf(full_path, sizeof(full_path), "%s/%s", gvfs_path, entry->d_name);

        if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode) &&
            (strstr(entry->d_name, "google-drive") != NULL ||
             strstr(entry->d_name, "dropbox") != NULL ||
             strstr(entry->d_name, "onedrive") != NULL ||
             strstr(entry->d_name, "mega") != NULL))
        {

            // Get file system information
            struct statvfs fs_info;
            if (statvfs(full_path, &fs_info) != 0)
            {
                continue;
            }

            // Calculate sizes
            unsigned long long total_bytes = (unsigned long long)fs_info.f_blocks * fs_info.f_frsize;
            unsigned long long available_bytes = (unsigned long long)fs_info.f_bavail * fs_info.f_frsize;
            unsigned long long used_bytes = total_bytes - available_bytes;

            // Format sizes for output
            char total_str[MAX_SIZE_STR_LENGTH], used_str[MAX_SIZE_STR_LENGTH], available_str[MAX_SIZE_STR_LENGTH];
            format_bytes(total_bytes, total_str, sizeof(total_str));
            format_bytes(used_bytes, used_str, sizeof(used_str));
            format_bytes(available_bytes, available_str, sizeof(available_str));

            // Target width calculation (same as before)
            int terminal_width = get_terminal_width();
            int box_width = terminal_width * TERMINAL_WIDTH_PERCENTAGE / TERMINAL_WIDTH_DIVISOR;
            if (box_width > MAX_BOX_WIDTH)
                box_width = MAX_BOX_WIDTH;
            if (box_width < MIN_BOX_WIDTH)
                box_width = MIN_BOX_WIDTH;
            int content_width = box_width - FRAME_PADDING;
            int bar_length = content_width - BRACKET_PADDING;
            if (bar_length < MIN_BAR_LENGTH)
                bar_length = MIN_BAR_LENGTH;

            // Calculate usage
            double usage_percent = calculate_usage_percent(total_bytes, available_bytes);
            int filled_length = (int)((usage_percent / USAGE_PERCENT_DIVISOR) * bar_length);
            char percent_text[MAX_PERCENT_TEXT_LENGTH];
            snprintf(percent_text, sizeof(percent_text), PERCENT_FORMAT, usage_percent);
            int text_length = strlen(percent_text);
            int text_start = filled_length > text_length ? (filled_length - text_length) / 2 : 0;

            // Create progress bar (same logic as before)
            size_t bar_bufsize = bar_length * MAX_BAR_BUFFER_MULTIPLIER + 1;
            char *bar = malloc(bar_bufsize);
            if (!bar)
            {
                perror("malloc");
                continue;
            }
            bar[0] = '\0';
            for (int i = 0; i < bar_length; i++)
            {
                if (i >= text_start && i < text_start + text_length && i < filled_length)
                {
                    get_bar_color(i, bar_length, colorbuf, sizeof(colorbuf));
                    int r, g, b;
                    sscanf(colorbuf, COLOR_FORMAT, &r, &g, &b);
                    char tmp[MAX_TEMP_BUFFER_LENGTH];
                    snprintf(tmp, sizeof(tmp), BACKGROUND_COLOR_FORMAT BLUE_TEXT_FORMAT "%c" RESET_FORMAT, r, g, b, BLUE_TEXT_R, BLUE_TEXT_G, BLUE_TEXT_B, percent_text[i - text_start]);
                    strncat(bar, tmp, bar_bufsize - strlen(bar) - 1);
                }
                else if (i < filled_length)
                {
                    get_bar_color(i, bar_length, colorbuf, sizeof(colorbuf));
                    strncat(bar, colorbuf, bar_bufsize - strlen(bar) - 1);
                    strncat(bar, "█" RESET_FORMAT, bar_bufsize - strlen(bar) - 1);
                }
                else
                {
                    strncat(bar, "\033[48;2;64;64;64m\033[38;2;160;160;160m░\033[0m", bar_bufsize - strlen(bar) - 1);
                }
            }

            // Determine cloud service name
            const char *service_name = "Cloud Storage";
            if (strstr(entry->d_name, "google-drive") != NULL)
            {
                service_name = "Google Drive";
            }
            else if (strstr(entry->d_name, "dropbox") != NULL)
            {
                service_name = "Dropbox";
            }
            else if (strstr(entry->d_name, "onedrive") != NULL)
            {
                service_name = "OneDrive";
            }
            else if (strstr(entry->d_name, "mega") != NULL)
            {
                service_name = "MEGA";
            }

            // Store information in drive_info_t structure
            drive_info_t *drive = &drives[*drive_count];
            fill_drive_info(drive, full_path, "fuse.gvfsd-fuse", entry->d_name,
                            NULL, NULL,
                            total_str, used_str, available_str,
                            total_bytes, used_bytes, available_bytes,
                            usage_percent, "Network Drive", bar, true, service_name, NULL,
                            (unsigned long long)fs_info.f_blocks, (unsigned long long)fs_info.f_files,
                            (double)fs_info.f_files / (double)fs_info.f_blocks);

            (*drive_count)++;
        }
    }

    closedir(dir);
}

void get_uuid_and_label(const char *device, char *uuid, size_t uuid_size, char *label, size_t label_size)
{
    uuid[0] = '\0';
    label[0] = '\0';

    char resolved_device[PATH_MAX];
    if (!realpath(device, resolved_device))
    {
        // Wenn das Gerät nicht auflösbar ist, abbrechen
        return;
    }

    // UUID suchen
    DIR *uuid_dir = opendir("/dev/disk/by-uuid/");
    if (uuid_dir)
    {
        struct dirent *entry;
        char full_path[PATH_MAX];
        char resolved_link[PATH_MAX];
        while ((entry = readdir(uuid_dir)) != NULL)
        {
            if (entry->d_name[0] == '.')
                continue;
            snprintf(full_path, sizeof(full_path), "/dev/disk/by-uuid/%s", entry->d_name);
            if (realpath(full_path, resolved_link))
            {
                if (strcmp(resolved_link, resolved_device) == 0)
                {
                    strncpy(uuid, entry->d_name, uuid_size - 1);
                    uuid[uuid_size - 1] = '\0';
                    break;
                }
            }
        }
        closedir(uuid_dir);
    }

    // Label suchen
    DIR *label_dir = opendir("/dev/disk/by-label/");
    if (label_dir)
    {
        struct dirent *entry;
        char full_path[PATH_MAX];
        char resolved_link[PATH_MAX];
        while ((entry = readdir(label_dir)) != NULL)
        {
            if (entry->d_name[0] == '.')
                continue;
            snprintf(full_path, sizeof(full_path), "/dev/disk/by-label/%s", entry->d_name);
            if (realpath(full_path, resolved_link))
            {
                if (strcmp(resolved_link, resolved_device) == 0)
                {
                    strncpy(label, entry->d_name, label_size - 1);
                    label[label_size - 1] = '\0';
                    break;
                }
            }
        }
        closedir(label_dir);
    }
}

// Helper function: Query SMART status (only for root and physical devices)
void get_smart_status(const char *device, char *status, size_t status_size)
{
    status[0] = '\0';
    if (geteuid() != 0)
        return;
    // Allow all /dev/sd*, /dev/nvme*, /dev/hd* (including partitions)
    if (!(
            strncmp(device, "/dev/sd", 7) == 0 ||
            strncmp(device, "/dev/nvme", 9) == 0 ||
            strncmp(device, "/dev/hd", 7) == 0))
        return;

    char cmd[256], line[256];
    snprintf(cmd, sizeof(cmd), "smartctl -H %s 2>/dev/null", device);
    FILE *fp = popen(cmd, "r");
    if (!fp)
        return;
    while (fgets(line, sizeof(line), fp))
    {
        if (strstr(line, "SMART overall-health self-assessment test result") || strstr(line, "SMART Health Status"))
        {
            char *p = strchr(line, ':');
            if (p)
            {
                p++;
                while (*p == ' ' || *p == '\t')
                    p++;
                strncpy(status, p, status_size - 1);
                status[status_size - 1] = '\0';
                char *nl = strchr(status, '\n');
                if (nl)
                    *nl = '\0';
                break;
            }
        }
        if (strstr(line, "PASSED"))
        {
            strncpy(status, "PASSED", status_size - 1);
            status[status_size - 1] = '\0';
            break;
        }
        if (strstr(line, "FAILED"))
        {
            strncpy(status, "FAILED", status_size - 1);
            status[status_size - 1] = '\0';
            break;
        }
        if (strstr(line, "UNKNOWN"))
        {
            strncpy(status, "UNKNOWN", status_size - 1);
            status[status_size - 1] = '\0';
            break;
        }
        if (strstr(line, "NOT AVAILABLE"))
        {
            strncpy(status, "NOT AVAILABLE", status_size - 1);
            status[status_size - 1] = '\0';
            break;
        }
    }
    pclose(fp);
}

int main(int argc, char *argv[])
{
    if (argc > 1)
    {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
        {
            show_help(argv[0]);
            return 0;
        }
        else if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0)
        {
            show_version();
            return 0;
        }
    }

    printf("\n");

    // Array to store all drive information
    drive_info_t drives[MAX_DRIVES];
    int drive_count = 0;

    // Open the mount table
    FILE *mtab = setmntent(MOUNT_TABLE_PATH, "r");
    if (mtab == NULL)
    {
        perror("Error opening mount table");
        return 1;
    }

    struct mntent *entry;

    // Loop through all mount points and collect drive information
    while ((entry = getmntent(mtab)) != NULL && drive_count < MAX_DRIVES)
    {
        // Skip special file systems
        const int skip_count = sizeof(skip_filesystems) / sizeof(skip_filesystems[0]);

        bool should_skip = false;
        for (int i = 0; i < skip_count; i++)
        {
            if (strcmp(entry->mnt_type, skip_filesystems[i]) == 0)
            {
                should_skip = true;
                break;
            }
        }

        if (should_skip)
        {
            continue;
        }

        // Show physical drives and network drives
        if (!is_physical_device(entry->mnt_fsname) &&
            !is_network_device(entry->mnt_fsname) &&
            !is_network_filesystem(entry->mnt_type))
        {
            continue;
        }

        // Skip AppImages and temporary mounts
        if (is_appimage_or_temp(entry->mnt_fsname, entry->mnt_dir))
        {
            continue;
        }

        // Get file system information
        struct statvfs fs_info;
        if (statvfs(entry->mnt_dir, &fs_info) != 0)
        {
            continue; // Skip if no information available
        }

        // Calculate sizes
        unsigned long long total_bytes = (unsigned long long)fs_info.f_blocks * fs_info.f_frsize;
        unsigned long long available_bytes = (unsigned long long)fs_info.f_bavail * fs_info.f_frsize;
        unsigned long long used_bytes = total_bytes - available_bytes;

        // Format sizes for output
        char total_str[MAX_SIZE_STR_LENGTH], used_str[MAX_SIZE_STR_LENGTH], available_str[MAX_SIZE_STR_LENGTH];
        format_bytes(total_bytes, total_str, sizeof(total_str));
        format_bytes(used_bytes, used_str, sizeof(used_str));
        format_bytes(available_bytes, available_str, sizeof(available_str));

        // Inode-Infos
        unsigned long long total_inodes = fs_info.f_files;
        unsigned long long free_inodes = fs_info.f_favail;
        unsigned long long used_inodes = total_inodes > 0 ? total_inodes - free_inodes : 0;
        double inode_usage = (total_inodes > 0) ? ((double)used_inodes / total_inodes) * 100.0 : 0.0;

        // Target width: 80% of terminal width or max. 120 characters
        int terminal_width = get_terminal_width();
        int box_width = terminal_width * TERMINAL_WIDTH_PERCENTAGE / TERMINAL_WIDTH_DIVISOR;
        if (box_width > MAX_BOX_WIDTH)
            box_width = MAX_BOX_WIDTH;
        if (box_width < MIN_BOX_WIDTH)
            box_width = MIN_BOX_WIDTH;
        int content_width = box_width - FRAME_PADDING;    // for frame
        int bar_length = content_width - BRACKET_PADDING; // for [ and ]
        if (bar_length < MIN_BAR_LENGTH)
            bar_length = MIN_BAR_LENGTH;

        // Calculate usage
        double usage_percent = calculate_usage_percent(total_bytes, available_bytes);
        int filled_length = (int)((usage_percent / USAGE_PERCENT_DIVISOR) * bar_length);
        char percent_text[MAX_PERCENT_TEXT_LENGTH];
        snprintf(percent_text, sizeof(percent_text), PERCENT_FORMAT, usage_percent);
        int text_length = strlen(percent_text);
        int text_start = filled_length > text_length ? (filled_length - text_length) / 2 : 0;

        // Dynamically allocate progress bar
        size_t bar_bufsize = bar_length * MAX_BAR_BUFFER_MULTIPLIER + 1;
        char *bar = malloc(bar_bufsize);
        if (!bar)
        {
            perror("malloc");
            continue;
        }
        bar[0] = '\0';
        for (int i = 0; i < bar_length; i++)
        {
            if (i >= text_start && i < text_start + text_length && i < filled_length)
            {
                get_bar_color(i, bar_length, colorbuf, sizeof(colorbuf));
                int r, g, b;
                sscanf(colorbuf, COLOR_FORMAT, &r, &g, &b);
                char tmp[MAX_TEMP_BUFFER_LENGTH];
                snprintf(tmp, sizeof(tmp), BACKGROUND_COLOR_FORMAT BLUE_TEXT_FORMAT "%c" RESET_FORMAT, r, g, b, BLUE_TEXT_R, BLUE_TEXT_G, BLUE_TEXT_B, percent_text[i - text_start]);
                strncat(bar, tmp, bar_bufsize - strlen(bar) - 1);
            }
            else if (i < filled_length)
            {
                get_bar_color(i, bar_length, colorbuf, sizeof(colorbuf));
                strncat(bar, colorbuf, bar_bufsize - strlen(bar) - 1);
                strncat(bar, "█" RESET_FORMAT, bar_bufsize - strlen(bar) - 1);
            }
            else
            {
                strncat(bar, "\033[48;2;64;64;64m\033[38;2;160;160;160m░\033[0m", bar_bufsize - strlen(bar) - 1);
            }
        }

        // Determine drive type
        const char *drive_type;
        if (is_physical_device(entry->mnt_fsname))
        {
            drive_type = "Local Drive";
        }
        else if (is_network_filesystem(entry->mnt_type) || is_network_device(entry->mnt_fsname))
        {
            drive_type = "Network Drive";
        }
        else
        {
            drive_type = "Other Drive";
        }

        // UUID und Label ermitteln
        char uuid[128], label[128];
        get_uuid_and_label(entry->mnt_fsname, uuid, sizeof(uuid), label, sizeof(label));

        // Store information in drive_info_t structure
        drive_info_t *drive = &drives[drive_count];
        fill_drive_info(drive, entry->mnt_dir, entry->mnt_type, entry->mnt_fsname,
                        uuid, label,
                        total_str, used_str, available_str,
                        total_bytes, used_bytes, available_bytes,
                        usage_percent, drive_type, bar, false, NULL, entry->mnt_opts,
                        total_inodes, used_inodes, inode_usage);

        drive_count++;
    }

    endmntent(mtab);

    // Check for GVFS-based cloud storage
    char gvfs_path[MAX_GVFS_PATH_LENGTH];
    snprintf(gvfs_path, sizeof(gvfs_path), GVFS_BASE_PATH, getuid());
    if (is_cloud_storage_directory(gvfs_path))
    {
        get_cloud_storage_info(gvfs_path, drives, &drive_count);
    }

    // Sort drives by capacity (largest first)
    qsort(drives, drive_count, sizeof(drive_info_t), compare_drives_by_capacity);

    // Display sorted drives
    for (int i = 0; i < drive_count; i++)
    {
        drive_info_t *drive = &drives[i];

        // Display drive information
        if (drive->is_cloud_storage)
        {
            printf("  " BOLD_YELLOW_FORMAT "Network Drive %d (%s)" RESET_COLOR_FORMAT "\n", i + 1, drive->cloud_service_name);
        }
        else
        {
            printf("  " BOLD_YELLOW_FORMAT "%s %d" RESET_COLOR_FORMAT "\n", drive->drive_type, i + 1);
        }
        printf("  Mount point:   %s\n", drive->mount_point);
        printf("  Filesystem:    %s\n", drive->filesystem);
        printf("  Device:        %s\n", drive->device);
        printf("  UUID:          %s\n", drive->uuid[0] ? drive->uuid : "-");
        printf("  Label:         %s\n", drive->label[0] ? drive->label : "-");
        printf("  Mount options: %s\n", drive->mount_options);
        printf("  Total size:    %s\n", drive->total_str);
        printf("  Used:          %s\n", drive->used_str);
        printf("  Available:     %s\n", drive->available_str);
        printf("  Inodes:        %llu/%llu (%.1f%% used)\n", drive->used_inodes, drive->total_inodes, drive->inode_usage);

        // SMART status only for root and physical devices
        if (geteuid() == 0 && !drive->is_cloud_storage && strcmp(drive->drive_type, "Local Drive") == 0)
        {
            char smart_status[128];
            get_smart_status(drive->device, smart_status, sizeof(smart_status));
            if (smart_status[0])
            {
                printf("  SMART:         %s\n", smart_status);
            }
            else
            {
                printf("  SMART:         No data\n");
            }
        }

        // Progress bar
        int terminal_width = get_terminal_width();
        int box_width = terminal_width * TERMINAL_WIDTH_PERCENTAGE / TERMINAL_WIDTH_DIVISOR;
        if (box_width > MAX_BOX_WIDTH)
            box_width = MAX_BOX_WIDTH;
        if (box_width < MIN_BOX_WIDTH)
            box_width = MIN_BOX_WIDTH;
        int content_width = box_width - FRAME_PADDING;

        int bar_visible_len = visible_length(drive->progress_bar);
        int bar_padding = content_width - bar_visible_len;
        printf("  %s%*s\n", drive->progress_bar, bar_padding, "");

        // Free allocated memory
        free(drive->progress_bar);
    }

    if (drive_count == 0)
    {
        printf("No drives found.\n");
    }
    else
    {
        printf("A total of %d drives found.\n", drive_count);
    }

    return 0;
}
