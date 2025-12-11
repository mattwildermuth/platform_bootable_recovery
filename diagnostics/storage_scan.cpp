#include <diagnostics/storage_scan.h>

#include <setjmp.h>
#include <signal.h>

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

static sigjmp_buf sigenv;
static int err_flag;

/*
 * TODO: try redrawing the screen twice and try calculating the time
 * it takes to it the second time (after no changes)
 */

#define MOCK_READ

/* cannot do the below because recoveryui is an abstract class :| */
/* static RecoveryUI ui; */
RecoveryUI* glob_ui;

static void segv_recover(int sig) {
  glob_ui->Print("2948739847239487234");
  err_flag = 1;
  sig = 1; /* stop compiler from complaining about unused var */
  siglongjmp(sigenv, sig);
  // siglongjmp(sigenv, 1);
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
  ui->PrintOnScreenOnly("Name%*s MB Read      Speed (MB/s)     Errors\n", (int)(longest_name - 4), " ");
}

static void print_dev_name(RecoveryUI* ui, size_t longest_name, struct dirent* dirent) {
  size_t name_len, indent_len;

  /* strlen should not return a negative here */
  name_len = strlen(dirent->d_name);
  indent_len = longest_name - name_len;

  ui->PrintOnScreenOnly("%s %*s", dirent->d_name, (int)indent_len, " ");
}

static int get_mb_read_padding(double mb_read, int longest_sz) {
  int num_chars = 0; /* num chars it takes to display the number */
  double padding_mb_read = mb_read;
  int longest_num_chars = 0;
  double padding_longest_sz = (double)longest_sz;

  for (; padding_longest_sz >= 1.0; longest_num_chars++)
    padding_longest_sz /= 10;
  for (; padding_mb_read >= 1.0; num_chars++)
    padding_mb_read /= 10;
  return (longest_num_chars - num_chars);
}

static void print_stats(RecoveryUI* ui, double mb_read, double longest_sz,
                        double time_reading, ssize_t num_errors) {
  double mb_per_sec;
  int mb_read_len, mb_per_sec_len;
  int mb_read_padding, mb_per_sec_padding;

  /* if the size read is less than a MB */
  mb_read_len = (int)std::log10(mb_read);
  if (mb_read_len < 0)
    mb_read_len = 0;

  mb_read_padding = ((int)std::log10(longest_sz)+1) - mb_read_len;
  mb_per_sec = (mb_read/time_reading);
  mb_per_sec_len = (int)std::log10(mb_per_sec);
  if (mb_per_sec_len < 0)
    mb_per_sec_len = 0;
  mb_per_sec_padding = 5 - mb_per_sec_len; /* should always be positive... */

  // ui->PrintOnScreenOnly("(%d-%d)", (int)std::log10(longest_sz), mb_read_len);

  ui->PrintOnScreenOnly("%.2f%*s%.2f%*s %zd\n", mb_read, mb_read_padding, " ", mb_per_sec, mb_per_sec_padding, " ", num_errors);
}

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
  // if (dev_pos % 1000 == 0.0 && dev_pos != 0)
  //   return -1;

  return read(fd, buf, count);
}

#endif /* #ifdef MOCK_READ */

/* assumes file position was initially at 0 */
static int get_file_sz(RecoveryUI* ui, int fd, off_t* sz) {
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

static double scan_device(RecoveryUI* ui, struct dirent* dirent, void* read_dst,
                          ssize_t* total_bytes_read, ssize_t* num_errors) {
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

  if (get_file_sz(ui, fd, &sz)) {
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
        ui->PrintOnScreenOnly("READ ERROR at MB #%zd\n", (fd_pos/READSZ));
      }
      (*num_errors)++;
      // ui->PrintOnScreenOnly("READ ERROR WHEN TRYING TO READ bytes: %zd, "
      //                       "MB #%zd (errno: %d, %s)\n",
      //                       total_bytes_read, (total_bytes_read/READSZ),
      //                       errno, strerror(errno));
      /*
       * the position of the file position pointer is undefined if
       * read returns an error -- we want to try and continue reading
       * -- hopefully the next block we've seeked to is readable
       */
      if (lseek(fd, (fd_pos + READSZ), SEEK_SET) == -1) {
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

  /*
   * technically, we could still have interval_bytes != 0 here -- we
   * shouldn't print anything though because an extra dot being
   * printed sometimes would be a weird thing to a user
   */

 seek_error:
  close(fd);

  return total_time;
}

static size_t get_longest_name(struct dirent** namelist, int num_devs) {
  size_t longest_name, name_len;
  longest_name = 0;
  for (int x = 0; x < num_devs; ++x) {
    name_len = strlen(namelist[x]->d_name);
    if (name_len >= longest_name)
      longest_name = name_len;
  }
  return longest_name;
}

static int get_longest_sz(RecoveryUI* ui, struct dirent** namelist,
                          int num_devs, double* longest_sz) {
  int fd;
  off_t sz;
  double sz_mb;
  std::string dir_path(BLKDEV_DIR);
  std::string full_path;

  *longest_sz = 0.0;
  for (int x = 0; x < num_devs; ++x) {
    full_path = dir_path + namelist[x]->d_name;
    if ((fd = open(full_path.c_str(), O_RDONLY)) == -1) {
      ui->PrintOnScreenOnly("couldn't be opened (errno: %d, %s)\n",
                            errno, strerror(errno));
      return 1;
    }

    if (get_file_sz(ui, fd, &sz)) {
      close(fd);
      return 1;
    }

    sz_mb = ((double)sz)/MB;
    if (sz_mb >= *longest_sz)
      *longest_sz = sz_mb;

    close(fd);
  }
  
  return 0;
}

static void do_scan_storage(RecoveryUI* ui, void* read_dst) {
  int num_devs;
  double longest_sz;
  size_t longest_name;
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

  longest_name = get_longest_name(namelist, num_devs);
  if (get_longest_sz(ui, namelist, num_devs, &longest_sz))
    return;

  // ui->PrintOnScreenOnly("longest_name: %zd, longest_sz: %.6f, ls pad: %d\n", longest_name, longest_sz, ((int)std::log10(longest_sz)));

  print_legend(ui, longest_name);

  for (int x = 0; x < num_devs; ++x) {
    bytes_read = 0;
    num_errors = 0;

    print_dev_name(ui, longest_name, namelist[x]);
    
    time_reading = scan_device(ui, namelist[x], read_dst, &bytes_read, &num_errors);

    if (time_reading < 0.0) {
      /* TODO: print *something* indicating error */
      break;
    } else if (time_reading > 0.0) {
      mb_read = ((double)bytes_read/MB);
      total_mb_read += mb_read;
      total_time_reading += time_reading;

      if (num_errors > 0)
        for (int x = 0; x < (longest_name + 1); x++)
          ui->PutChar(' ');

      print_stats(ui, mb_read, longest_sz, time_reading, num_errors);
    }
  }

  for (int x = 0; x < num_devs; ++x)
    free(namelist[x]);
  free(namelist);

  ui->PrintOnScreenOnly("Average read speed: %.2f MB/s\n",
                        (total_mb_read/total_time_reading));
}

static int sig_hndlr_ini(int sig, void (*handler)(int), struct sigaction* oldsa) {
  struct sigaction sa;

  sa.sa_flags = SA_RESTART;
  sa.sa_handler = handler;
  sigemptyset(&sa.sa_mask);

  if (sigaction(sig, &sa, oldsa) < 0)
    return 1;

  return 0;
}

void scan_storage(RecoveryUI* ui) {
  void* read_dst;

  glob_ui = ui;

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

  struct sigaction oldsa;
  sigset_t unblock_segv_set, prev_set;
  if (sigaddset(&unblock_segv_set, SIGSEGV))
    ui->Print("segaddset failed");
  if (sigprocmask(SIG_UNBLOCK, &unblock_segv_set, &prev_set))
    ui->Print("sigprocmask failed");

  if (sig_hndlr_ini(SIGSEGV, segv_recover, &oldsa))
    ui->Print("AHHHHH");

  int* zed = 0;
  if (!sigsetjmp(sigenv, 0))
    (*zed) = 6;
  else
    ui->Print("other side of the jmp\n");

  ui->Print("err_flag: %d\n", err_flag);

  // do_scan_storage(ui, read_dst);

  // if (!sigsetjmp(sigenv, 0))
  //   do_scan_storage(ui, read_dst);
  // else
  //   ui->Print("FATAL\n");

  sigaction(SIGSEGV, &oldsa, 0);
  sigprocmask(SIG_SETMASK, &prev_set, 0);
}
