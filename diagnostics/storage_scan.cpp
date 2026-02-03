#include "diagnostics/storage_scan.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cmath>

#define MB (1024 * 1024)
#define READ_SIZE (MB)
#define BLK_DEVICE_DIR "/dev/block/by-name/"

#define FASTEST_READ_SPEED 9999.99 // (MB/s)

#define LEGEND_COLSEP 4
#define LEGEND_NAME "Name"
#define LEGEND_BYTES_READ "MB Read"
#define LEGEND_SPEED "Speed (MB/s)"
#define LEGEND_ERRORS "Errors"

static RecoveryUI* ui;

// TODO: REVIEW: this is an exact duplicate of the (also) static
// function in recovery_ui/screen_ui.cpp. Consider engineering
// something to remove this duplicate definition
static double now() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec + tv.tv_usec / 1000000.0;
}

static void print_legend(int longest_name, int longest_size,
                         int longest_speed, int longest_error) {
  ui->PrintOnScreenOnly("%-*s %*s%*s%*s%*s%*s\n",
                        longest_name, LEGEND_NAME,
                        longest_size, LEGEND_BYTES_READ,
                        LEGEND_COLSEP, "",
                        longest_speed, LEGEND_SPEED,
                        LEGEND_COLSEP, "",
                        longest_error, LEGEND_ERRORS);
}

static void print_dev_name(int longest_name, struct dirent* dirent) {
  ui->PrintOnScreenOnly("%-*s ", longest_name, dirent->d_name);
}

static void print_dev_name_spacing(int longest_name) {
  ui->PrintOnScreenOnly("%-*s ", longest_name, "");
}

static void print_stats(double mb_read, int longest_size, double time_reading,
                        int longest_speed, ssize_t num_errors, int longest_error) {
  ui->PrintOnScreenOnly("%*.2f%*s%*.2f%*s%*zd\n",
                        longest_size, mb_read,
                        LEGEND_COLSEP, "",
                        longest_speed, (mb_read/time_reading),
                        LEGEND_COLSEP, "",
                        longest_error, num_errors);
}

static int get_file_size(int fd, off_t* size) {
  // assumes file position was initially at 0
  if ((*size = lseek(fd, 0, SEEK_END)) == -1) {
    ui->PrintOnScreenOnly("could not seek to end of device "
                          "(errno: %d, %s)\n", errno, strerror(errno));
    return 1;
  }
  if (lseek(fd, 0, SEEK_SET) == -1) {
    ui->PrintOnScreenOnly("could not seek back to start of device "
                          "(errno: %d, %s)\n", errno, strerror(errno));
    return 1;
  }
  return 0;
}

static int get_longest_name(struct dirent** namelist, int num_devs) {
  size_t longest_name, name_len;

  longest_name = strlen(LEGEND_NAME);
  for (int x = 0; x < num_devs; ++x) {
    name_len = strlen(namelist[x]->d_name);
    if (name_len >= longest_name)
      longest_name = name_len;
  }
  return (int)longest_name;
}

static int get_longest_size(struct dirent** namelist, int num_devs, int* longest_size) {
  int fd, min_read_len;
  off_t size;
  double size_mb, largest_size;
  std::string dir_path(BLK_DEVICE_DIR);
  std::string full_path;

  largest_size = 0.0;
  for (int x = 0; x < num_devs; ++x) {
    full_path = dir_path + namelist[x]->d_name;
    if ((fd = open(full_path.c_str(), O_RDONLY)) == -1) {
      ui->PrintOnScreenOnly("couldn't be opened (errno: %d, %s)\n",
                            errno, strerror(errno));
      return 1;
    }

    if (get_file_size(fd, &size)) {
      close(fd);
      return 1;
    }

    size_mb = ((double)size)/MB;
    if (size_mb >= largest_size)
      largest_size = size_mb;

    close(fd);
  }

  // +1 because log starts 'counting' at 0
  *longest_size = (int)std::log10(largest_size) + 1;
  if (*longest_size <= 0) {
    *longest_size = 1;
  }

  *longest_size += 3; // +3 for decimal precision

  // TODO: should we fix the strlen return? if it's 'bigger' than an
  // int, it'll be interpreted as negative -- *highly* unlikely
  min_read_len = (int)strlen(LEGEND_BYTES_READ);
  if (*longest_size < min_read_len)
    *longest_size = min_read_len;

  return 0;
}

static int get_longest_speed() {
  int longest_speed, min_speed_len;
  // +1 because log starts 'counting' at 0
  // +3 to keep track of decimal precision characters
  longest_speed = (int)std::log10(FASTEST_READ_SPEED)+4;
  // TODO: should we fix the strlen return? if it's 'bigger' than an
  // int, it'll be interpreted as negative -- *highly* unlikely
  min_speed_len = (int)strlen(LEGEND_SPEED);

  if (longest_speed < min_speed_len)
    longest_speed = min_speed_len;
  return longest_speed;
}

// TODO: debatably, shouldn't be a function, but kept to preserve the
// 'getting max length' pattern
static int get_longest_error() {
  return (int)strlen(LEGEND_ERRORS);
}

static int blk_device_compare(const struct dirent** dirent_a, const struct dirent** dirent_b) {
  int compar_result;
  struct stat dirent_a_statbuf, dirent_b_statbuf;
  std::string a_path(BLK_DEVICE_DIR);
  std::string b_path(BLK_DEVICE_DIR);
  unsigned int a_operand, b_operand;

  a_path += (*dirent_a)->d_name;
  b_path += (*dirent_b)->d_name;

  // TODO: What do we do on stat error?
  //
  // It's not possible to exit to the menu from this function
  // 
  // It may be more complex than it's worth to communicate the error
  // to somewhere else in the code which can exit to the menu
  //
  // Current thinking is to return a result for the sorter and if
  // there's an actual problem reading from the device later, we'll
  // find out and exit properly (incl. cleanup)
  if (stat(a_path.c_str(), &dirent_a_statbuf) == -1) {
    ui->PrintOnScreenOnly("COULD NOT STAT %s (errno: %d, %s)\n",
                          a_path.c_str(), errno, strerror(errno));
    return 0;
  }
  if (stat(b_path.c_str(), &dirent_b_statbuf) == -1) {
    ui->PrintOnScreenOnly("COULD NOT STAT %s (errno: %d, %s)\n",
                          b_path.c_str(), errno, strerror(errno));
    return 0;
  }

  a_operand = major(dirent_a_statbuf.st_dev);
  b_operand = major(dirent_b_statbuf.st_dev);

  if (a_operand == b_operand) {
    a_operand = minor(dirent_a_statbuf.st_dev);
    b_operand = minor(dirent_b_statbuf.st_dev);
  }

  // if both major and minor are equal, at least sort alphabetically
  if (a_operand == b_operand) {
    compar_result = alphasort(dirent_a, dirent_b);
  } else if (a_operand > b_operand) {
    compar_result =  1;
  } else {
    compar_result = -1;
  }

  return compar_result;
}

static int blk_device_filter(const struct dirent* dirent) {
  struct stat dirent_statbuf;
  std::string dev_path(BLK_DEVICE_DIR);
  dev_path += dirent->d_name;

  // stat helps get the target file type even if the dirent is a sym
  // link
  if (stat(dev_path.c_str(), &dirent_statbuf) == -1) {
    ui->PrintOnScreenOnly("COULD NOT STAT %s in scandir filter (errno: %d, %s)\n",
                          dev_path.c_str(), errno, strerror(errno));
    return 0;
  }
  return S_ISBLK(dirent_statbuf.st_mode);
}

static double scan_device(struct dirent* dirent, void* read_dest,
                          ssize_t* total_bytes_read, ssize_t* num_errors) {
  int fd;
  ssize_t bytes_read;
  double total_time, before_read_time;
  off_t size, fd_pos;

  std::string full_path(BLK_DEVICE_DIR);
  full_path += dirent->d_name;

  if ((fd = open(full_path.c_str(), O_RDONLY)) == -1) {
    ui->PrintOnScreenOnly("couldn't be opened (errno: %d, %s)\n",
                          errno, strerror(errno));
    return 0.0;
  }

  *total_bytes_read = 0;
  total_time = 0.0;

  if (get_file_size(fd, &size)) {
    goto seek_error;
  }

  bytes_read = 0;
  *num_errors = 0;
  fd_pos = 0;

  while (true) {
    before_read_time = now();
    bytes_read = read(fd, read_dest, READ_SIZE);
    total_time += (now() - before_read_time);

    if (bytes_read == 0) {
      break;
    } else if (bytes_read == -1) {
      if (*num_errors < 5) {
        if (*num_errors == 0)
          ui->PrintOnScreenOnly("\n");
        ui->PrintOnScreenOnly("  [%s] off: %ld (%d)\n",
                              strerrorname_np(errno), fd_pos, READ_SIZE);
      }
      (*num_errors)++;

      // The position of the file position pointer is undefined if
      // read returns an error -- we want to try and continue reading
      // -- hopefully the next block we've seeked to is readable
      if (lseek(fd, (fd_pos + READ_SIZE), SEEK_SET) == -1) {
        ui->PrintOnScreenOnly("Could not continue to read file after "
                              "read error -- left in undefined "
                              "position (errno: %d, %s)\n", errno,
                              strerror(errno));
        goto seek_error;
      }
      bytes_read = 0;
    }
    if (ui->IsKeyPressed(KEY_VOLUMEDOWN)) {
      ui->PrintOnScreenOnly("\n");
      return -1.0;
    }

    *total_bytes_read += bytes_read;
    fd_pos += READ_SIZE;
  }

 seek_error:
  close(fd);

  return total_time;
}

void scan_storage(RecoveryUI* current_ui) {
  int longest_name, longest_size, longest_speed, longest_error;

  void* read_dest;
  double mb_read, total_mb_read;
  ssize_t bytes_read, num_errors;
  double time_reading, total_time_reading;

  int num_devs;
  struct dirent** namelist;

  ui = current_ui;

  ui->ClearText();

  // TODO: print is currently one character larger than a line on the pixel 6a :|
  ui->PrintOnScreenOnly("Scanning block devices in %s for bad sectors\n"
                        "\n\nHold volume down to cancel\n\n", BLK_DEVICE_DIR);

  // mmap here to properly align the buffer for faster writes
  read_dest = mmap(0, READ_SIZE, PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
  if (read_dest == MAP_FAILED) {
    ui->PrintOnScreenOnly("Could not allocate space to dump the "
                          "read bytes into (errno: %d, %s)\n", errno,
                          strerror(errno));
    return;
  }

  total_mb_read = 0.0;
  total_time_reading = 0.0;

  num_devs = scandir(BLK_DEVICE_DIR, &namelist, blk_device_filter, blk_device_compare);
  if (num_devs < 0) {
    ui->PrintOnScreenOnly("ERROR: could not scan %s (errno: %d, %s)",
                          BLK_DEVICE_DIR, errno, strerror(errno));
    return;
  }

  longest_name = get_longest_name(namelist, num_devs);
  if (get_longest_size(namelist, num_devs, &longest_size))
    return;
  longest_speed = get_longest_speed();
  longest_error = get_longest_error();

  print_legend(longest_name, longest_size, longest_speed, longest_error);

  for (int x = 0; x < num_devs; ++x) {
    bytes_read = 0;
    num_errors = 0;

    print_dev_name(longest_name, namelist[x]);

    time_reading = scan_device(namelist[x], read_dest, &bytes_read, &num_errors);

    if (time_reading < 0.0) {
      // This is the case where volume down is pressed and we quit early
      break;
    } else if (time_reading > 0.0) {
      mb_read = ((double)bytes_read/MB);
      total_mb_read += mb_read;
      total_time_reading += time_reading;

      if (num_errors > 0)
        print_dev_name_spacing(longest_name);

      print_stats(mb_read, longest_size, time_reading,
                  longest_speed, num_errors, longest_error);
    }
  }

  for (int x = 0; x < num_devs; ++x)
    free(namelist[x]);
  free(namelist);

  ui->PrintOnScreenOnly("Average read speed: %.2f MB/s\n",
                        (total_mb_read/total_time_reading));
}
