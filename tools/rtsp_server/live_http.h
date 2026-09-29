/* live_http.h - embedded live HTTP video streaming for rtspd
 *
 * A small standalone HTTP server (enabled with `-D <port>`) that re-serves
 * the H.264 bitstream from the gmlib encoder as fMP4 (Media Source
 * Extensions) and HLS. The camera's lighttpd mod_cgi buffers the entire
 * CGI stdout until the process exits, so progressive video can not go
 * through the PHP API; this module binds its own port instead.
 */
#ifndef LIVE_HTTP_H
#define LIVE_HTTP_H

/* Start the HTTP listener thread. port <= 0 disables it. width/height/fps
 * are taken from the encoder configuration (cliArgs) for the codec config
 * box. Returns 0 on success, -1 on failure. */
int live_http_init(int port, int width, int height, int fps);

/* Stop the listener and drop all connection threads. */
void live_http_stop(void);

/* Feed one encoded H.264 access unit (Annex-B bitstream) into the ring.
 * pts_ms is the gmlib timestamp in milliseconds. keyframe != 0 marks an
 * IDR / open-GOP boundary. Safe to call from the encoder thread. */
void live_http_feed_video(const unsigned char *buf, int len,
                          unsigned int pts_ms, int keyframe);

/* True once the server is listening (feed can be gated on this). */
int live_http_enabled(void);

#endif /* LIVE_HTTP_H */