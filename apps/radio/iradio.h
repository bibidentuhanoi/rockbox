/***************************************************************************
 *   Internet Radio for ESP32 Rockbox port
 ***************************************************************************/
#ifndef IRADIO_H
#define IRADIO_H

#ifdef HAVE_IRADIO

#define IRADIO_MAX_STATIONS 50
#define IRADIO_MAX_URL_LEN  256
#define IRADIO_MAX_NAME_LEN 64

int iradio_screen(void);

#endif /* HAVE_IRADIO */
#endif /* IRADIO_H */
