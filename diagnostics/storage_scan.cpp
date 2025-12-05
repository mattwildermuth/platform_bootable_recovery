#include <diagnostics/storage_scan.h>

#include <string.h>

#include <sys/types.h>
#include <fcntl.h>

#include <sys/stat.h>

#include <unistd.h>

#include <dirent.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>

#include <cmath>

#define MB (1024 * 1024)
#define READSZ (MB)
#define BLKDEV_DIR "/dev/block/by-name/"

/* in MB/s */
#define FASTEST_READ_SPEED 9999.99

#define LEGEND_COLSEP 4
#define LEGEND_NAME "Name"
#define LEGEND_BYTES_READ "MB Read"
#define LEGEND_SPEED "Speed (MB/s)"
#define LEGEND_ERRORS "Errors"

/*
 * TODO: try redrawing the screen twice and try calculating the time
 * it takes to it the second time (after no changes)
 */

#define MOCK_READ

static RecoveryUI* ui;

/*
 * TODO: REVIEW: this is an exact duplicate of the function in
 * recovery_ui/screen_ui.cpp. Consider engineering something to remove
 * this duplicate definition
 */
static double now() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec + tv.tv_usec / 1000000.0;
}

#ifdef MOCK_READ

static int mocked_read(int fd, void* buf, size_t count) {
  off_t dev_pos;
  double dev_pos_mb;

  if ((dev_pos = lseek(fd, 0, SEEK_CUR)) == -1)
    return -1;

  dev_pos_mb = ((double)dev_pos/MB);
  if (std::fmod(dev_pos_mb, 1000.0) == 0.0 && dev_pos != 0)
    return -1;

  return read(fd, buf, count);
}

#endif /* #ifdef MOCK_READ */

static void print_legend(int longest_name, int longest_sz,
                         int longest_speed, int longest_error) {
  ui->PrintOnScreenOnly("%-*s %*s%*s%*s%*s%*s\n",
                        longest_name, LEGEND_NAME,
                        longest_sz, LEGEND_BYTES_READ,
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

static void print_stats(double mb_read, int longest_sz, double time_reading,
                        int longest_speed, ssize_t num_errors, int longest_error) {
  ui->PrintOnScreenOnly("%*.2f%*s%*.2f%*s%*zd\n",
                        longest_sz, mb_read,
                        LEGEND_COLSEP, "",
                        longest_speed, (mb_read/time_reading),
                        LEGEND_COLSEP, "",
                        longest_error, num_errors);
}

/* assumes file position was initially at 0 */
static int get_file_sz(int fd, off_t* sz) {
  if ((*sz = lseek(fd, 0, SEEK_END)) == -1) {
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

static int get_longest_sz(struct dirent** namelist, int num_devs, int* longest_sz) {
  int fd;
  int min_read_len;
  off_t sz;
  double sz_mb, largest_sz;
  std::string dir_path(BLKDEV_DIR);
  std::string full_path;

  largest_sz = 0.0;
  for (int x = 0; x < num_devs; ++x) {
    full_path = dir_path + namelist[x]->d_name;
    if ((fd = open(full_path.c_str(), O_RDONLY)) == -1) {
      ui->PrintOnScreenOnly("couldn't be opened (errno: %d, %s)\n",
                            errno, strerror(errno));
      return 1;
    }

    if (get_file_sz(fd, &sz)) {
      close(fd);
      return 1;
    }

    sz_mb = ((double)sz)/MB;
    if (sz_mb >= largest_sz)
      largest_sz = sz_mb;

    close(fd);
  }

  /* +1 because log starts 'counting' at 0 */
  *longest_sz = (int)std::log10(largest_sz) + 1;
  if (*longest_sz <= 0) {
    *longest_sz = 1;
  }

  *longest_sz += 3; /* +3 for decimal precision */

  /*
   * TODO: fix the strlen return if it's 'bigger' than an int and is
   * interpreted as negative -- *highly* unlikely
   */
  min_read_len = (int)strlen(LEGEND_BYTES_READ);
  if (*longest_sz < min_read_len)
    *longest_sz = min_read_len;

  return 0;
}

static int get_longest_speed() {
  int longest_speed, min_speed_len;
  /*
   * +1 because log starts 'counting' at 0
   * +3 to keep track of decimal precision characters
   */
  longest_speed = (int)std::log10(FASTEST_READ_SPEED)+4;
  /*
   * TODO: fix the strlen return if it's 'bigger' than an int and is
   * interpreted as negative -- *highly* unlikely
   */
  min_speed_len = (int)strlen(LEGEND_SPEED);

  if (longest_speed < min_speed_len)
    longest_speed = min_speed_len;
  return longest_speed;
}

/*
 * debatably, shouldn't be a function, but kept to preserve the
 * 'getting max length' pattern
 */
static int get_longest_error() {
  return (int)strlen(LEGEND_ERRORS);
}

static int blkdev_compar(const struct dirent** dirent_a, const struct dirent** dirent_b) {
  int compar_result;
  struct stat dirent_a_statbuf, dirent_b_statbuf;
  std::string a_path(BLKDEV_DIR);
  std::string b_path(BLKDEV_DIR);
  unsigned int a_operand, b_operand;

  a_path += (*dirent_a)->d_name;
  b_path += (*dirent_b)->d_name;

  /*
   * TODO: What do we do on stat error?
   *
   * Could we read the device if this fails?
   * How do we communicate that an error happend up the chain?
   * Exiting here would feel extreme, but is it even possible to do
   *   that in this 'callback'?
   */
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

  /* if equal, at least sort alphabetically */
  if (a_operand == b_operand) {
    compar_result = alphasort(dirent_a, dirent_b);
  } else if (a_operand > b_operand) {
    compar_result =  1;
  } else {
    compar_result = -1;
  }

  return compar_result;
}

/*
 * TODO: this does not yet check if the block dev on the other side of
 * the link is a block device
 */
static int blkdev_filter(const struct dirent* dirent) {
  return dirent->d_type == DT_BLK || dirent->d_type == DT_LNK;
}

static double scan_device(struct dirent* dirent, void* read_dst,
                          ssize_t* total_bytes_read, ssize_t* num_errors,
                          int longest_name, int longest_sz) {
  int fd;
  ssize_t bytes_read;
  double total_time, before_read_time;
  off_t sz, fd_pos;

  std::string full_path(BLKDEV_DIR);
  full_path += dirent->d_name;

  if ((fd = open(full_path.c_str(), O_RDONLY)) == -1) {
    ui->PrintOnScreenOnly("couldn't be opened (errno: %d, %s)\n",
                          errno, strerror(errno));
    return 0.0;
  }

  *total_bytes_read = 0;
  total_time = 0.0;

  if (get_file_sz(fd, &sz)) {
    goto seek_error;
  }

  bytes_read = 0;
  *num_errors = 0;
  fd_pos = 0;

  while (true) {
    before_read_time = now();
#ifdef MOCK_READ
    bytes_read = mocked_read(fd, read_dst, READSZ);
#else
    bytes_read = read(fd, read_dst, READSZ);
#endif
    total_time += (now() - before_read_time);

    if (bytes_read == 0) {
      break;
    } else if (bytes_read == -1) {
      if (*num_errors < 5) {
        if (*num_errors == 0)
          ui->PutChar('\n');
        ui->PrintOnScreenOnly("%*s offset: %ld size: %d\n%*s %s\n",
                              (longest_name+longest_sz+1), "READ ERROR:",
                              fd_pos, READSZ,
                              (longest_name+longest_sz+1), "errno str:", strerror(errno));
      }
      (*num_errors)++;
      /*
       * the position of the file position pointer is undefined if
       * read returns an error -- we want to try and continue reading
       * -- hopefully the next block we've seeked to is readable
       */
      if (lseek(fd, (fd_pos + READSZ), SEEK_SET) == -1) {
        // if (*num_errors == 0)
        //   print_dev_name_spacing(ui, longest_name);
        ui->PrintOnScreenOnly("Could not continue to read file after "
                              "read error -- left in undefined "
                              "position (errno: %d, %s)\n", errno,
                              strerror(errno));
        goto seek_error;
      }
      bytes_read = 0;
    }
    if (ui->IsKeyPressed(KEY_VOLUMEDOWN)) {
      ui->PutChar('\n');
      return -1.0;
    }

    *total_bytes_read += bytes_read;
    fd_pos += READSZ;
  }

 seek_error:
  close(fd);

  return total_time;
}

void scan_storage(RecoveryUI* current_ui) {
  int longest_name, longest_sz, longest_speed, longest_error;

  void* read_dst;
  double mb_read, total_mb_read;
  ssize_t bytes_read, num_errors;
  double time_reading, total_time_reading;

  int num_devs;
  struct dirent** namelist;

  ui = current_ui;

  ui->ClearText();

  ui->PrintOnScreenOnly("Scanning block devices in %s for bad sectors\n"
                        "\n\nHold volume down to cancel\n\n", BLKDEV_DIR);

  /* mmap here to properly align the buffer for faster writes */
  read_dst = mmap(0, READSZ, PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
  if (read_dst == MAP_FAILED) {
    ui->PrintOnScreenOnly("Could not allocate space to dump the "
                          "read bytes into (errno: %d, %s)\n", errno,
                          strerror(errno));
    return;
  }

  total_mb_read = 0.0;
  total_time_reading = 0.0;

  num_devs = scandir(BLKDEV_DIR, &namelist, blkdev_filter, blkdev_compar);
  if (num_devs < 0) {
    ui->PrintOnScreenOnly("ERROR: could not scan %s (errno: %d, %s)",
                          BLKDEV_DIR, errno, strerror(errno));
    return;
  }

  longest_name = get_longest_name(namelist, num_devs);
  if (get_longest_sz(namelist, num_devs, &longest_sz))
    return;
  longest_speed = get_longest_speed();
  longest_error = get_longest_error();

  print_legend(longest_name, longest_sz, longest_speed, longest_error);

  for (int x = 0; x < num_devs; ++x) {
    bytes_read = 0;
    num_errors = 0;

    print_dev_name(longest_name, namelist[x]);
    
    time_reading = scan_device(namelist[x], read_dst, &bytes_read,
                               &num_errors, longest_name, longest_sz);

    if (time_reading < 0.0) {
      /* This is just the case where we volume down and quit early */
      break;
    } else if (time_reading > 0.0) {
      mb_read = ((double)bytes_read/MB);
      total_mb_read += mb_read;
      total_time_reading += time_reading;

      if (num_errors > 0)
        print_dev_name_spacing(longest_name);

      print_stats(mb_read, longest_sz, time_reading, longest_speed, num_errors, longest_error);
    }
  }

  for (int x = 0; x < num_devs; ++x)
    free(namelist[x]);
  free(namelist);

  ui->PrintOnScreenOnly("Average read speed: %.2f MB/s\n",
                        (total_mb_read/total_time_reading));
}
