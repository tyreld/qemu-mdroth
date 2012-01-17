#include <glib.h>

#ifdef _WIN32
typedef HANDLE GAHandle;
#else
typedef int GAHandle;
#endif

typedef struct GAChannel GAChannel;

typedef gboolean (*GAChannelCallback)(GIOCondition condition, gpointer data);

GAChannel *ga_channel_new(GAHandle handle, GIOCondition condition, GAChannelCallback cb, gpointer user_data);
void ga_channel_close(GAChannel *c);
GIOStatus ga_channel_read(GAChannel *c, char *buf, size_t size, gsize *count);
GIOStatus ga_channel_write_all(GAChannel *c, const char *buf, size_t size);
