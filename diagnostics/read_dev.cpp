#include <diagnostics/read_dev.h>

#include <sys/types.h>
#include <fcntl.h>

#include <sys/stat.h>

#include <unistd.h>

#include <dirent.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>

#define READSZ (1024 * 1024)
#define BLKDEV_DIR "/dev/block/by-name/"
#define BLKDEV_DIR_STR (str(BLKDEV_DIR))

static int blah(RecoveryUI* ui, struct dirent* dirent, void* read_dst, size_t longest_name) {
  int fd;
  ssize_t total_bytes_read, bytes_read, interval_bytes;
  ssize_t update_interval, sz;
  ssize_t ident_len, name_len;

  std::string full_path(BLKDEV_DIR);
  full_path += dirent->d_name;

  name_len = strlen(dirent->d_name);
  if (name_len >= longest_name)
    ident_len = 0;
  else
    ident_len = longest_name - name_len;

  ui->Print("%s ", dirent->d_name);
  for (int x = 0; x < ident_len; x++)
    ui->Print(" ");

  if ((fd = open(full_path.c_str(), O_RDONLY)) == -1)
  {
    ui->Print("couldn't be opened\n");
    return 0;
  }

  /* TODO: CHECK LSEEK OUTPUT FOR ERROR */
  sz = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);

  update_interval = sz/20;
  bytes_read = 0;
  interval_bytes = 0;
  total_bytes_read = 0;

  while ((bytes_read = read(fd, read_dst, READSZ)) > 0)
  {
    if (ui->IsKeyPressed(KEY_VOLUMEDOWN))
      return 1;
    total_bytes_read += bytes_read;
    interval_bytes += bytes_read;
    while (interval_bytes >= update_interval)
    {
      interval_bytes -= update_interval;
      ui->Print(". ");
    }
  }

  /* 
   * technically, we could still have interval_bytes != 0 here -- we
   * shouldn't print anything though because an extra dot being
   * printed sometimes would be a weird thing to a user
   */

  if (total_bytes_read != sz)
    ui->Print("Only read %zd out of %zd total bytes",
              total_bytes_read, sz);

  ui->Print("\n");

  close(fd);
  return 0;
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
  int num_devs;
  size_t longest_name, name_len;
  struct dirent** namelist;

  /*
   * https://www.gnu.org/software/libc/manual/html_node/Accessing-Directories.html
   *
   * scandir(3)
   */
  num_devs = scandir(BLKDEV_DIR, &namelist, blkdev_filter, blkdev_compar);
  if (num_devs < 0)
  {
    ui->Print("ERROR: could not scan %s", BLKDEV_DIR);
    return;
  }

  longest_name = 0;
  for (int x = 0; x < num_devs; ++x)
  {
    name_len = strlen(namelist[x]->d_name);
    if (name_len >= longest_name)
      longest_name = name_len;
  }

  // ui->Print("\nLongest name: %zd\n", longest_name);

  /* TODO: rethink freeing due to early exit from blah */
  for (int x = 0; x < num_devs; ++x)
  {
    if (blah(ui, namelist[x], read_dst, longest_name))
      break;
  }

  for (int x = 0; x < num_devs; ++x)
    free(namelist[x]);
  free(namelist);
}

void read_block_devices(RecoveryUI* ui) {
  void* read_dst;

  ui->Print("Reading all block devices listed in %s\n"
            "to find any bad sectors\n\n", BLKDEV_DIR);

  /* mmap here to properly align the buffer for faster writes */
  read_dst = mmap(0, READSZ, PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
  if (read_dst == MAP_FAILED)
  {
    ui->Print("Could not allocate space to dump the read bytes "
              "into: %d\n", errno);
    return;
  }

  do_read_block_devices(ui, read_dst);
}
