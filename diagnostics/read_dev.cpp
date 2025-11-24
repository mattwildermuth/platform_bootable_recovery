#include <diagnostics/read_dev.h>

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

#define SPEED_AFTER_EVERY_DEV
#define MOCK_READ

/* cannot do the below because recoveryui is an abstract class :| */
// static RecoveryUI ui;

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

static double read_dev(RecoveryUI* ui, struct dirent* dirent, void* read_dst, size_t longest_name) {
  int fd;
  ssize_t total_bytes_read, bytes_read, interval_bytes;
  ssize_t update_interval, sz;
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

  if ((fd = open(full_path.c_str(), O_RDONLY)) == -1)
  {
    ui->PrintOnScreenOnly("couldn't be opened (errno: %d, %s)\n",
                          errno, strerror(errno));
    return 0.0;
  }

  if ((sz = lseek(fd, 0, SEEK_END)) == -1)
  {
    ui->PrintOnScreenOnly("could not seek to end of device "
                          "(errno: %d, %s)\n", errno, strerror(errno));
    return 0.0;
  }
  if (lseek(fd, 0, SEEK_SET) == -1)
  {
    ui->PrintOnScreenOnly("could not seek back to start of device "
                          "(errno: %d, %s)\n", errno, strerror(errno));
    return 0.0;
  }

  update_interval = sz/20;
  bytes_read = 0;
  interval_bytes = 0;
  total_bytes_read = 0;
  total_time = 0.0;

  while (true)
  {
    before_read_time = now();
    bytes_read = read(fd, read_dst, READSZ);
    total_time += (now() - before_read_time);

    /* TODO: handle error case (bytes_read = -1) and check errno */
    if (bytes_read == 0)
    {
      break;
    }
    else if (bytes_read == -1)
    {
      //
    }
    if (ui->IsKeyPressed(KEY_VOLUMEDOWN))
    {
      ui->PutChar('\n');
      return -1.0;
    }

    total_bytes_read += bytes_read;
    interval_bytes += bytes_read;
    while (interval_bytes >= update_interval)
    {
      interval_bytes -= update_interval;
      ui->PutChar('.');
      ui->PutChar(' ');
    }
    ui->Redraw();
  }

  /* 
   * technically, we could still have interval_bytes != 0 here -- we
   * shouldn't print anything though because an extra dot being
   * printed sometimes would be a weird thing to a user
   */

  if (total_bytes_read != sz)
    ui->PrintOnScreenOnly("Only read %zd out of %zd total bytes",
                          total_bytes_read, sz);

  /*
   * Debatable whether we should call Print() here to force a redraw
   * if this function is to be truly generic
   */
  ui->PutChar('\n');

  close(fd);

  return ((total_bytes_read/MB)/total_time);
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
  if (stat(a_path.c_str(), &dirent_a_statbuf) == -1)
  {
    // ui->PrintOnScreenOnly("COULD NOT STAT %s (errno: %d, %s)\n",
    //                       a_path.c_str(), errno, strerror(errno));
    return 0;
  }
  if (stat(b_path.c_str(), &dirent_b_statbuf) == -1)
  {
    // ui->PrintOnScreenOnly("COULD NOT STAT %s (errno: %d, %s)\n",
    //                       b_path.c_str(), errno, strerror(errno));
    return 0;
  }

  a_operand = major(dirent_a_statbuf.st_dev);
  b_operand = major(dirent_b_statbuf.st_dev);

  if (a_operand == b_operand)
  {
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

static void do_scan_storage(RecoveryUI* ui, void* read_dst) {
  int num_devs, num_devs_read;
  size_t longest_name, name_len;
  struct dirent** namelist;
  double avg_speed, file_speed;

  num_devs_read = 0;
  avg_speed = 0.0;

  /*
   * https://www.gnu.org/software/libc/manual/html_node/Accessing-Directories.html
   *
   * scandir(3)
   */
  num_devs = scandir(BLKDEV_DIR, &namelist, blkdev_filter, blkdev_compar);
  if (num_devs < 0)
  {
    ui->PrintOnScreenOnly("ERROR: could not scan %s (errno: %d, %s)",
                          BLKDEV_DIR, errno, strerror(errno));
    return;
  }

  longest_name = 0;
  for (int x = 0; x < num_devs; ++x)
  {
    name_len = strlen(namelist[x]->d_name);
    if (name_len >= longest_name)
      longest_name = name_len;
  }

  for (int x = 0; x < num_devs; ++x)
  {
    file_speed = read_dev(ui, namelist[x], read_dst, longest_name);
    if (file_speed < 0.0)
    {
      break;
    }
    else if (file_speed > 0.0)
    {
#ifdef SPEED_AFTER_EVERY_DEV
      ui->PrintOnScreenOnly("Read speed (MB/s): %f\n", file_speed);
#endif
      num_devs_read++;
      avg_speed += file_speed;
    }
  }

  for (int x = 0; x < num_devs; ++x)
    free(namelist[x]);
  free(namelist);

  ui->PrintOnScreenOnly("Average read speed (MB/s): %f\n",
                        (avg_speed/num_devs_read));
}

void scan_storage(RecoveryUI* ui) {
  void* read_dst;

  ui->ClearText();

  ui->PrintOnScreenOnly("Reading all block devices listed in %s\n"
                        "to find any bad sectors\n\n", BLKDEV_DIR);

  /* mmap here to properly align the buffer for faster writes */
  read_dst = mmap(0, READSZ, PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
  if (read_dst == MAP_FAILED)
  {
    ui->PrintOnScreenOnly("Could not allocate space to dump the "
                          "read bytes into (errno: %d, %s)\n", errno,
                          strerror(errno));
    return;
  }

  do_scan_storage(ui, read_dst);
}
