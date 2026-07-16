#ifndef GLOBAL_H
#define GLOBAL_H

/* Mirror/audio receiver capabilities. Direct AirPlay video and HLS are not
 * advertised because this project does not implement URL-based playback. */
#define GLOBAL_FEATURES_1 0x5A7FFEE6U
#define GLOBAL_FEATURES_2 0x00000000U
#define GLOBAL_MODEL    "AppleTV14,1"
#define GLOBAL_VERSION  "845.5.1"
#define GLOBAL_DISPLAY_REFRESH_RATE 60
#define GLOBAL_DISPLAY_MAX_FPS      60

#define MAX_HWADDR_LEN 6

#endif
