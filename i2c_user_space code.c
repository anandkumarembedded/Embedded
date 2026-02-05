#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <math.h>

int main(void)
{
    int fd;
    char buf[128];
    int n;
    int temp, pressure;
    double altitude;

    fd = open("/dev/i2c_sensor", O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    while (1) {
        memset(buf, 0, sizeof(buf));

        lseek(fd, 0, SEEK_SET);
        n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0)
            continue;

        /*
         * Expected kernel output format:
         * "Temp: %d C  Pressure: %d Pa\n"
         */
        if (sscanf(buf, "Temp: %d C  Pressure: %d Pa",
                   &temp, &pressure) == 2) {

            /* Altitude calculation (barometric formula) */
            altitude = 44330.0 *
                       (1.0 - pow((double)pressure / 101325.0, 0.1903));

            printf("Temp: %d C  Pressure: %d Pa  Altitude: %.2f m\n",
                   temp, pressure, altitude);
        }

        sleep(2);   /* sampling interval */
    }

    close(fd);
    return 0;
}


