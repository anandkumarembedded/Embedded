#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main(void)
{
    int fd = open("/dev/rpi_loopback", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    while (1) {
        char cmd;
        char buf[64];
        ssize_t n;
        int connected, val;

        printf("Enter 1 or 0: ");
        scanf(" %c", &cmd);

        if (write(fd, &cmd, 1) < 0)
            perror("write");

        memset(buf, 0, sizeof(buf));
        n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) {
            perror("read");
            continue;
        }

        buf[n] = '\0';

        if (sscanf(buf, "%d %d", &connected, &val) != 2) {
            printf("Invalid data from driver: %s\n", buf);
            continue;
        }

        if (!connected) {
            printf("Device is disconnected\n\n");
        } else if (val) {
            printf("1 : LED's ON , Device is connected\n\n");
        } else {
            printf("0 : LED's OFF, Device is connected\n\n");
        }
    }
}
