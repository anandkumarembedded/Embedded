#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>

#define DEV "/dev/gpio_ctrl"

#define GPIO_SET_PIN     _IOW('G', 1, int)
#define GPIO_SET_DIR     _IOW('G', 2, int)
#define GPIO_GET_VAL     _IOR('G', 3, int)
#define GPIO_RESET_CNT  _IO('G', 4)

int main()
{
    int fd, choice, gpio, val;
    char cmd;

    fd = open(DEV, O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    while (1) {
        printf("\n1.Set GPIO\n2.Set Direction\n3.LED ON\n4.LED OFF\n");
        printf("5.Read GPIO\n6.Blocking Read Button\n7.Reset Counter\n0.Exit\n");
        scanf("%d", &choice);

        switch (choice) {
        case 1:
            printf("GPIO number: ");
            scanf("%d", &gpio);
            ioctl(fd, GPIO_SET_PIN, &gpio);
            break;

        case 2:
            printf("1=OUT, 0=IN: ");
            scanf("%d", &val);
            ioctl(fd, GPIO_SET_DIR, &val);
            break;

        case 3:
            cmd = '1';
            write(fd, &cmd, 1);
            break;

        case 4:
            cmd = '0';
            write(fd, &cmd, 1);
            break;

        case 5:
            ioctl(fd, GPIO_GET_VAL, &val);
            printf("GPIO Value: %d\n", val);
            break;

        case 6:
            read(fd, &val, sizeof(val));
            printf("Button Press Count: %d\n", val);
            break;

        case 7:
            ioctl(fd, GPIO_RESET_CNT);
            break;

        case 0:
            close(fd);
            return 0;
        }
    }
}
