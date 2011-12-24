#include <glib.h>

typedef struct GAChannel GAChannel;

typedef gboolean (*GAChannelCallback)(GIOCondition condition, gpointer data);

GAChannel *ga_channel_new(int fd, GIOCondition condition, GAChannelCallback cb, gpointer user_data);
GIOStatus ga_channel_read(GAChannel *c, char *buf, int size, gsize *count);
GIOStatus ga_channel_write_all(GAChannel *c, const char *buf, int size, gsize *count);
