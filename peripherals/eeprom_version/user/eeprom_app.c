#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#define EEPROM_DEV "/dev/eeprom_ver"

int main(int argc, char *argv[])
{
    int fd;
    int ret;
    char buf[32] = {0};

    fd = open(EEPROM_DEV, O_RDWR);
    if (fd < 0) {
        perror("Failed to open eeprom device");
        return -1;
    }

    if (argc > 1) {
        ret = write(fd, argv[1], strlen(argv[1]));
        if (ret < 0) {
            perror("Failed to write data");
            close(fd);
            return -1;
        }
        usleep(10000);
        lseek(fd, 0, SEEK_SET);
    }

    ret = read(fd, buf, sizeof(buf) - 1);
    if (ret < 0) {
        perror("Failed to read version");
        close(fd);
        return -1;
    }

    buf[ret] = '\0';
    printf("EEPROM Version: %s\n", buf);
    close(fd);
    return 0;
}
