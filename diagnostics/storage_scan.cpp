#include <diagnostics/storage_scan.h>

#include <string.h>

#include <sys/types.h>
#include <fcntl.h>

#include <sys/stat.h>

#include <unistd.h>

#include <dirent.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>

#define MB (1024 * 1024)
#define READSZ (MB)
#define BLKDEV_DIR "/dev/block/by-name/"

/*
 * TODO: try redrawing the screen twice and try calculating the time
 * it takes to it the second time (after no changes)
 */

// #define MOCK_READ

/* cannot do the below because recoveryui is an abstract class :| */
/* static RecoveryUI ui; */

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

  if ((dev_pos = lseek(fd, 0, SEEK_CUR)) == -1)
    return -1;

  if (((double)dev_pos/MB) == 1000.0)
    return -1;

  return read(fd, buf, count);
}

#endif /* #ifdef MOCK_READ */

static double storage_scan(RecoveryUI* ui, struct dirent* dirent, void* read_dst,
                           size_t longest_name, ssize_t* total_bytes_read, ssize_t* num_errors) {
  int fd;
  bool printed_error;
  ssize_t bytes_read, sz;
  ssize_t indent_len, name_len;
  double total_time, before_read_time;

  std::string full_path(BLKDEV_DIR);
  full_path += dirent->d_name;

  name_len = strlen(dirent->d_name);
  if (name_len >= longest_name)
    indent_len = 0;
  else
    indent_len = longest_name - name_len;

  /* Use PutChar to avoid redrawing the screen */
  for (int x = 0; x < name_len; x++)
    ui->PutChar(dirent->d_name[x]);
  for (int x = 0; x < indent_len; x++)
    ui->PutChar(' ');

  ui->PutChar(' ');
  ui->Redraw();

  if ((fd = open(full_path.c_str(), O_RDONLY)) == -1) {
    ui->PrintOnScreenOnly("couldn't be opened (errno: %d, %s)\n",
                          errno, strerror(errno));
    return 0.0;
  }

  *total_bytes_read = 0;
  total_time = 0.0;

  if ((sz = lseek(fd, 0, SEEK_END)) == -1) {
    ui->PrintOnScreenOnly("could not seek to end of device "
                          "(errno: %d, %s)\n", errno, strerror(errno));
    goto seek_error;
  }
  if (lseek(fd, 0, SEEK_SET) == -1) {
    ui->PrintOnScreenOnly("could not seek back to start of device "
                          "(errno: %d, %s)\n", errno, strerror(errno));
    goto seek_error;
  }

  bytes_read = 0;
  *num_errors = 0;
  printed_error = false;

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
    }
    else if (bytes_read == -1) {
      (*num_errors)++;
      if (!printed_error) {
        printed_error = true;
        ui->PrintOnScreenOnly("READ ERROR at MB #%zd\n", (*total_bytes_read/READSZ));
        for (int x = 0; x < (indent_len + name_len + 1); x++)
          ui->PutChar(' ');
        ui->Redraw();
      }
      // ui->PrintOnScreenOnly("READ ERROR WHEN TRYING TO READ bytes: %zd, "
      //                       "MB #%zd (errno: %d, %s)\n",
      //                       total_bytes_read, (total_bytes_read/READSZ),
      //                       errno, strerror(errno));
      /*
       * the position of the file position pointer is undefined if
       * read returns an error -- we want to try and continue reading
       * -- hopefully the next block we've seeked to is readable
       */
      if (lseek(fd, (*total_bytes_read + READSZ), SEEK_SET) == -1) {
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
  }

  /*
   * technically, we could still have interval_bytes != 0 here -- we
   * shouldn't print anything though because an extra dot being
   * printed sometimes would be a weird thing to a user
   */

 seek_error:
  close(fd);

  return total_time;
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
   * TODO: What do we do on error?
   *
   * Could we read the device if this fails?
   * How screwed are we if stat fails?
   * How do we communicate that an error happend up the chain?
   * Exiting here would feel extreme, but is it even possible to do
   *   that in this 'callback'?
   *
   * the ui variable is also not available here atm -- is it worth
   * even trying to make global if we can't even bail?
   *
   * Hopefully, if we haven't managed to stat here and something else
   * is wrong, it'll be picked up by another function later on down
   * the line when the file is being opened or read from
   */
  if (stat(a_path.c_str(), &dirent_a_statbuf) == -1) {
    // ui->PrintOnScreenOnly("COULD NOT STAT %s (errno: %d, %s)\n",
    //                       a_path.c_str(), errno, strerror(errno));
    return 0;
  }
  if (stat(b_path.c_str(), &dirent_b_statbuf) == -1) {
    // ui->PrintOnScreenOnly("COULD NOT STAT %s (errno: %d, %s)\n",
    //                       b_path.c_str(), errno, strerror(errno));
    return 0;
  }

  a_operand = major(dirent_a_statbuf.st_dev);
  b_operand = major(dirent_b_statbuf.st_dev);

  if (a_operand == b_operand) {
    a_operand = minor(dirent_a_statbuf.st_dev);
    b_operand = minor(dirent_b_statbuf.st_dev);
  }

  /* if equal, at least sort alphabetically */
  if (a_operand == b_operand)
    compar_result = alphasort(dirent_a, dirent_b);
  else if (a_operand > b_operand)
    compar_result =  1;
  else
    compar_result = -1;

  return compar_result;
}

static int blkdev_filter(const struct dirent* dirent) {
  return dirent->d_type == DT_BLK || dirent->d_type == DT_LNK;
}

static void print_legend(RecoveryUI* ui, size_t longest_name) {
  int name_padding;
  ui->PrintOnScreenOnly("Name");

  /* we should be fine here -- we will almost certainly have an 'sda' */
  name_padding = (longest_name - 4);
  for (int x = 0; x < (name_padding + 1); x++)
    ui->PutChar(' ');

  ui->PrintOnScreenOnly("MB Read      Speed (MB/s)     Errors\n");
}

static void print_stats(RecoveryUI* ui, double mb_read, double time_reading, ssize_t num_errors) {
  double padding_mb_read, mb_per_sec;

  /* TODO: fix -- this code hurts my eyes */

  padding_mb_read = mb_read;
  // ui->PrintOnScreenOnly("%.2f MB ", mb_read);
  ui->PrintOnScreenOnly("%.2f ", mb_read);
  /* the above statement will produce at least 3 chars -- we want to pad that*/
  while (padding_mb_read < 1.0) {
    padding_mb_read *= 10;
  }
  /* pad with maximum 3 characters (beyond 1GB is not padded) */
  while (padding_mb_read < 1000.0) {
    padding_mb_read *= 10;
    ui->PutChar(' ');
  }
  ui->PutChar(' ');
  ui->PutChar(' ');
  ui->PutChar(' ');
  ui->PutChar(' ');
  ui->PutChar(' ');

  mb_per_sec = (mb_read/time_reading);
  // ui->PrintOnScreenOnly("(%.4f MB/s), ", mb_per_sec);
  ui->PrintOnScreenOnly("%.4f ", mb_per_sec);
  while (mb_per_sec < 1000.0) {
    mb_per_sec *= 10;
    ui->PutChar(' ');
  }
  ui->PutChar(' ');
  ui->PutChar(' ');
  ui->PutChar(' ');
  ui->PutChar(' ');
  ui->PutChar(' ');
  ui->PutChar(' ');
  ui->PutChar(' ');
  ui->PrintOnScreenOnly("%zd\n", num_errors);
}

static void do_scan_storage(RecoveryUI* ui, void* read_dst) {
  int num_devs;
  size_t longest_name, name_len;
  ssize_t bytes_read, total_mb_read;
  ssize_t num_errors;
  struct dirent** namelist;
  double time_reading, total_time_reading;
  double mb_read, padding_mb_read, mb_per_sec;

  total_mb_read = 0;
  total_time_reading = 0.0;

  /*
   * https://www.gnu.org/software/libc/manual/html_node/Accessing-Directories.html
   *
   * scandir(3)
   */
  num_devs = scandir(BLKDEV_DIR, &namelist, blkdev_filter, blkdev_compar);
  if (num_devs < 0) {
    ui->PrintOnScreenOnly("ERROR: could not scan %s (errno: %d, %s)",
                          BLKDEV_DIR, errno, strerror(errno));
    return;
  }

  longest_name = 0;
  for (int x = 0; x < num_devs; ++x) {
    name_len = strlen(namelist[x]->d_name);
    if (name_len >= longest_name)
      longest_name = name_len;
  }

  print_legend(ui, longest_name);

  for (int x = 0; x < num_devs; ++x) {
    bytes_read = 0;
    num_errors = 0;
    time_reading = storage_scan(ui, namelist[x], read_dst,
                                longest_name, &bytes_read, &num_errors);

    if (time_reading < 0.0) {
      break;
    }
    else if (time_reading > 0.0) {
      mb_read = ((double)bytes_read/MB);
      total_mb_read += mb_read;
      total_time_reading += time_reading;

      print_stats(ui, mb_read, time_reading, num_errors);
    }
  }

  for (int x = 0; x < num_devs; ++x)
    free(namelist[x]);
  free(namelist);

  ui->PrintOnScreenOnly("Average read speed: %.4f MB/s\n",
                        (total_mb_read/total_time_reading));
}

void scan_storage(RecoveryUI* ui) {
  void* read_dst;

  ui->ClearText();

  ui->PrintOnScreenOnly("Reading all block devices listed in %s\n"
                        "to find any bad sectors\n\n", BLKDEV_DIR);

  /* mmap here to properly align the buffer for faster writes */
  read_dst = mmap(0, READSZ, PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
  if (read_dst == MAP_FAILED) {
    ui->PrintOnScreenOnly("Could not allocate space to dump the "
                          "read bytes into (errno: %d, %s)\n", errno,
                          strerror(errno));
    return;
  }

  do_scan_storage(ui, read_dst);
}
