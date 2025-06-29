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

// Constants for terminal and display
#define TERM_FALLBACK_WIDTH 80
#define VERSION "1.0.1"
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

char colorbuf[COLOR_BUFFER_SIZE]; // For bar colors

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

int is_physical_device(const char *fsname)
{
    // Check for /dev/sd*, /dev/nvme*, /dev/hd*
    return (strncmp(fsname, "/dev/sd", DEV_SD_PREFIX_LEN) == 0 ||
            strncmp(fsname, "/dev/nvme", DEV_NVME_PREFIX_LEN) == 0 ||
            strncmp(fsname, "/dev/hd", DEV_HD_PREFIX_LEN) == 0);
}

int is_network_device(const char *fsname)
{
    // Check for network file systems
    return (strncmp(fsname, "//", NETWORK_PATH_PREFIX_LEN) == 0 ||   // SMB/CIFS shares
            strncmp(fsname, "\\\\", NETWORK_PATH_PREFIX_LEN) == 0 || // Windows network paths
            strstr(fsname, ":") != NULL);                            // NFS and other network protocols
}

int is_network_filesystem(const char *fstype)
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

int is_appimage_or_temp(const char *fsname, const char *mountpoint)
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
int is_cloud_storage_directory(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir)
        return 0;

    struct dirent *entry;
    int has_cloud_storage = 0;

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
            has_cloud_storage = 1;
            break;
        }
    }

    closedir(dir);
    return has_cloud_storage;
}

// Function to get cloud storage info from GVFS
void get_cloud_storage_info(const char *gvfs_path, int *drive_count)
{
    DIR *dir = opendir(gvfs_path);
    if (!dir)
        return;

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL)
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

            // Output
            printf("  " BOLD_YELLOW_FORMAT "Network Drive %d (%s)" RESET_COLOR_FORMAT "\n", *drive_count + 1, service_name);
            printf("  Mount point:   %s\n", full_path);
            printf("  Filesystem:    %s\n", "fuse.gvfsd-fuse");
            printf("  Device:        %s\n", entry->d_name);
            printf("  Total size:    %s\n", total_str);
            printf("  Used:          %s\n", used_str);
            printf("  Available:     %s\n", available_str);

            int bar_visible_len = visible_length(bar);
            int bar_padding = content_width - bar_visible_len;
            printf("  %s%*s\n", bar, bar_padding, "");

            free(bar);
            (*drive_count)++;
        }
    }

    closedir(dir);
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
    // Open the mount table
    FILE *mtab = setmntent(MOUNT_TABLE_PATH, "r");
    if (mtab == NULL)
    {
        perror("Error opening mount table");
        return 1;
    }

    struct mntent *entry;
    int drive_count = 0;

    // Loop through all mount points
    while ((entry = getmntent(mtab)) != NULL)
    {
        // Skip special file systems
        if (strcmp(entry->mnt_type, "proc") == 0 ||
            strcmp(entry->mnt_type, "sysfs") == 0 ||
            strcmp(entry->mnt_type, "devpts") == 0 ||
            strcmp(entry->mnt_type, "tmpfs") == 0 ||
            strcmp(entry->mnt_type, "devtmpfs") == 0 ||
            strcmp(entry->mnt_type, "securityfs") == 0 ||
            strcmp(entry->mnt_type, "cgroup") == 0 ||
            strcmp(entry->mnt_type, "cgroup2") == 0 ||
            strcmp(entry->mnt_type, "pstore") == 0 ||
            strcmp(entry->mnt_type, "efivarfs") == 0 ||
            strcmp(entry->mnt_type, "autofs") == 0 ||
            strcmp(entry->mnt_type, "debugfs") == 0 ||
            strcmp(entry->mnt_type, "tracefs") == 0 ||
            strcmp(entry->mnt_type, "configfs") == 0 ||
            strcmp(entry->mnt_type, "fusectl") == 0 ||
            strcmp(entry->mnt_type, "fuse.gvfsd-fuse") == 0 ||
            strcmp(entry->mnt_type, "binfmt_misc") == 0)
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
            exit(1);
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
        size_t barline_bufsize = bar_bufsize + BARLINE_BUFFER_EXTRA;
        char *barline = malloc(barline_bufsize);
        if (!barline)
        {
            perror("malloc");
            free(bar);
            exit(1);
        }
        snprintf(barline, barline_bufsize, "[%-*s]", bar_length, ""); // Placeholder for padding
        snprintf(barline, barline_bufsize, "[%-*s]", bar_length, ""); // for padding

        // Content
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
        printf("  " BOLD_YELLOW_FORMAT "%s %d" RESET_COLOR_FORMAT "\n", drive_type, drive_count + 1);
        printf("  Mount point:   %s\n", entry->mnt_dir);
        printf("  Filesystem:    %s\n", entry->mnt_type);
        printf("  Device:        %s\n", entry->mnt_fsname);
        printf("  Total size:    %s\n", total_str);
        printf("  Used:          %s\n", used_str);
        printf("  Available:     %s\n", available_str);
        // Progress bar
        int bar_visible_len = visible_length(bar);
        int bar_padding = content_width - bar_visible_len;
        printf("  %s%*s\n", bar, bar_padding, "");

        free(bar);
        free(barline);
        drive_count++;
    }

    endmntent(mtab);

    // Check for GVFS-based cloud storage
    char gvfs_path[MAX_GVFS_PATH_LENGTH];
    snprintf(gvfs_path, sizeof(gvfs_path), GVFS_BASE_PATH, getuid());
    if (is_cloud_storage_directory(gvfs_path))
    {
        get_cloud_storage_info(gvfs_path, &drive_count);
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
