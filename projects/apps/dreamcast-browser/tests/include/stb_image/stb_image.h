#ifndef IMAGE_TEST_STB_IMAGE_H
#define IMAGE_TEST_STB_IMAGE_H
int stbi_info_from_memory(const unsigned char *data, int length,
                          int *width, int *height, int *channels);
unsigned char *stbi_load_from_memory(const unsigned char *data, int length,
                                     int *width, int *height, int *channels,
                                     int requested_channels);
void stbi_image_free(void *pixels);
const char *stbi_failure_reason(void);
#endif
