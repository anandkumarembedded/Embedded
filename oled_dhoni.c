#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define FB_SIZE (OLED_WIDTH * OLED_HEIGHT / 8)
/*
 Simple 5x7 font (only required letters)
 Each byte = one column (LSB at top)
*/
static const uint8_t font[5][5] = {
    // D
    {0x7F, 0x41, 0x41, 0x22, 0x1C},
    // H
    {0x7F, 0x08, 0x08, 0x08, 0x7F},
    // O
    {0x3E, 0x41, 0x41, 0x41, 0x3E},
    // N
    {0x7F, 0x10, 0x08, 0x04, 0x7F},
    // I
    {0x00, 0x41, 0x7F, 0x41, 0x00}
};
void draw_char(uint8_t *fb, int x, int y, const uint8_t *glyph)
{
}
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            if (glyph[col] & (1 << row)) {
                int px = x + col;
                int py = y + row;
                int index = px + (py / 8) * OLED_WIDTH;
                fb[index] |= (1 << (py % 8));
            }
        }
    }
int main()
{
    int fd = open("/dev/oled", O_WRONLY);
    uint8_t fb[FB_SIZE];
    memset(fb, 0x00, FB_SIZE); // Clear screen
    int x = 10;  // start position
    int y = 20;
    for (int i = 0; i < 5; i++) {
        draw_char(fb, x + i * 6, y, font[i]);
    }
    write(fd, fb, FB_SIZE);
    close(fd);
    return 0;
}
