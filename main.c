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

char colorbuf[32]; // For bar colors

// Function to get terminal width
int get_terminal_width()
{
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0)
    {
        return w.ws_col;
    }
    return 80; // Fallback-Value
}

// Function to format bytes into human-readable sizes
void format_bytes(unsigned long long bytes, char *buffer, size_t buffer_size)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double size = bytes;

    while (size >= 1024.0 && unit_index < 4)
    {
        size /= 1024.0;
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
    return ((double)used / total) * 100.0;
}

int is_physical_device(const char *fsname)
{
    // Check for /dev/sd*, /dev/nvme*, /dev/hd*
    return (strncmp(fsname, "/dev/sd", 7) == 0 ||
            strncmp(fsname, "/dev/nvme", 9) == 0 ||
            strncmp(fsname, "/dev/hd", 7) == 0);
}

// Helper for true-color gradient (red-yellow-green)
void get_bar_color(int idx, int max, char *buffer, size_t size)
{
    // New gradient: 0% = green (0,255,0), 50% = yellow (255,255,0), 100% = red (255,0,0)
    float ratio = (float)idx / (float)(max - 1);
    int r, g, b;
    if (ratio < 0.5f)
    {
        // Green to Yellow
        r = (int)(ratio * 2 * 255);
        g = 255;
        b = 0;
    }
    else
    {
        // Yellow to Red
        r = 255;
        g = (int)((1.0f - (ratio - 0.5f) * 2) * 255);
        b = 0;
    }
    snprintf(buffer, size, "\033[38;2;%d;%d;%dm", r, g, b);
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

// Helper: find UUID from /dev/disk/by-uuid
void get_uuid(const char *device, char *uuid, size_t uuid_size)
{
    uuid[0] = '\0';
    FILE *fp = popen("ls -l /dev/disk/by-uuid/", "r");
    if (!fp)
        return;
    char line[512];
    while (fgets(line, sizeof(line), fp))
    {
        char *p = strstr(line, "> ");
        if (p && strstr(line, device))
        {
            char *start = line;
            while (*start == ' ')
                start++;
            char *end = strchr(start, ' ');
            if (end && (end - start) < (int)uuid_size)
            {
                strncpy(uuid, start, end - start);
                uuid[end - start] = '\0';
                break;
            }
        }
    }
    pclose(fp);
}

// Helper: mount time (ctime of mountpoint)
void get_mount_time(const char *mountpoint, char *buf, size_t bufsize)
{
    struct stat st;
    if (stat(mountpoint, &st) == 0)
    {
        struct tm *tm = localtime(&st.st_ctime);
        strftime(buf, bufsize, "%Y-%m-%d %H:%M:%S", tm);
    }
    else
    {
        snprintf(buf, bufsize, "unknown");
    }
}

int main()
{
    printf("\n");
    // Open the mount table
    FILE *mtab = setmntent("/proc/mounts", "r");
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
        // Only show physical drives
        if (!is_physical_device(entry->mnt_fsname))
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
        char total_str[64], used_str[64], available_str[64];
        format_bytes(total_bytes, total_str, sizeof(total_str));
        format_bytes(used_bytes, used_str, sizeof(used_str));
        format_bytes(available_bytes, available_str, sizeof(available_str));

        // Target width: 80% of terminal width or max. 120 characters
        int terminal_width = get_terminal_width();
        int box_width = terminal_width * 4 / 5;
        if (box_width > 120)
            box_width = 120;
        if (box_width < 40)
            box_width = 40;
        int content_width = box_width - 4;  // for frame
        int bar_length = content_width - 2; // for [ and ]
        if (bar_length < 10)
            bar_length = 10;

        // Calculate usage
        double usage_percent = calculate_usage_percent(total_bytes, available_bytes);
        int filled_length = (int)((usage_percent / 100.0) * bar_length);
        char percent_text[16];
        snprintf(percent_text, sizeof(percent_text), "%.1f%%", usage_percent);
        int text_length = strlen(percent_text);
        int text_start = filled_length > text_length ? (filled_length - text_length) / 2 : 0;

        // Dynamically allocate progress bar
        size_t bar_bufsize = bar_length * 64 + 1;
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
                sscanf(colorbuf, "\033[38;2;%d;%d;%dm", &r, &g, &b);
                char tmp[128];
                snprintf(tmp, sizeof(tmp), "\033[48;2;%d;%d;%dm\033[38;2;0;0;255m%c\033[0m", r, g, b, percent_text[i - text_start]);
                strncat(bar, tmp, bar_bufsize - strlen(bar) - 1);
            }
            else if (i < filled_length)
            {
                get_bar_color(i, bar_length, colorbuf, sizeof(colorbuf));
                strncat(bar, colorbuf, bar_bufsize - strlen(bar) - 1);
                strncat(bar, "█\033[0m", bar_bufsize - strlen(bar) - 1);
            }
            else
            {
                strncat(bar, "\033[48;2;64;64;64m\033[38;2;160;160;160m░\033[0m", bar_bufsize - strlen(bar) - 1);
            }
        }
        size_t barline_bufsize = bar_bufsize + 16;
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
        printf("  \033[1;33mDrive %d\033[0m\n", drive_count + 1);
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
