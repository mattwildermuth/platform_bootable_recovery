#include <diagnostics/read_dev.h>

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

  ui->Redraw();

  if ((fd = open(full_path.c_str(), O_RDONLY)) == -1)
  {
    ui->PrintOnScreenOnly("couldn't be opened\n");
    return 0.0;
  }

  /* TODO: CHECK LSEEK OUTPUT FOR ERROR */
  sz = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);

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
      break;
    if (ui->IsKeyPressed(KEY_VOLUMEDOWN))
      return -1.0;

    total_bytes_read += bytes_read;
    interval_bytes += bytes_read;
    while (interval_bytes >= update_interval)
    {
      interval_bytes -= update_interval;
      ui->PrintOnScreenOnly(". ");
    }
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
   * TODO: think about error checking here -- maybe we just return
   * less than or equal or something
   *
   * but still scream and print the errno
   */
  stat(a_path.c_str(), &dirent_a_statbuf);
  stat(b_path.c_str(), &dirent_b_statbuf);

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

static void do_read_block_devices(RecoveryUI* ui, void* read_dst) {
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
    ui->PrintOnScreenOnly("ERROR: could not scan %s", BLKDEV_DIR);
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
      num_devs_read++;
      avg_speed += file_speed;
    }
  }

  for (int x = 0; x < num_devs; ++x)
    free(namelist[x]);
  free(namelist);

  ui->PrintOnScreenOnly("Average read speed (Mb/s): %f",
                        (avg_speed/num_devs_read));
}

void read_block_devices(RecoveryUI* ui) {
  void* read_dst;

  ui->ClearText();

  ui->PrintOnScreenOnly("Reading all block devices listed in %s\n"
                        "to find any bad sectors\n\n", BLKDEV_DIR);

  /* mmap here to properly align the buffer for faster writes */
  read_dst = mmap(0, READSZ, PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
  if (read_dst == MAP_FAILED)
  {
    ui->PrintOnScreenOnly("Could not allocate space to dump the "
                          "read bytes into: %d\n", errno);
    return;
  }

  do_read_block_devices(ui, read_dst);
}
