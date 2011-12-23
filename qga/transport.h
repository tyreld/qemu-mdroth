#include <glib.h>

typedef struct QgaChannel QgaChannel;

typedef gboolean (*QgaChannelCallback)(GIOCondition condition, gpointer data);

QgaChannel *qga_channel_new(int fd, GIOCondition condition, QgaChannelCallback cb, gpointer user_data);
GIOStatus qga_channel_read(QgaChannel *c, char *buf, int size, gsize *count);
GIOStatus qga_channel_write_all(QgaChannel *c, const char *buf, int size, gsize *count);
