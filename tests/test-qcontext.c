#include <glib.h>
#include "qom/object.h"
#include "qemu/module.h"
#include "qcontext/qcontext.h"
#include "qemu/event_notifier.h"

#if 0
typedef struct TestEventState {
    QSource *qsource;
    GPollFD poll_fds[8];
    int n_poll_fds;
    bool dispatched;
    bool skip_poll;
#define CB_VALUE_PASS 42
#define CB_VALUE_FAIL 0
    int cb_value;
} TestEventState;

static bool test_event_prepare(QSource *evt, int *timeout)
{
    QSourceClass *evtk = QSOURCE_GET_CLASS(evt);
    TestEventState *s = evtk->get_user_data(evt);

    return s->skip_poll;
}

static bool test_event_check(QSource *qevt)
{
    QSourceClass *qevtk = QSOURCE_GET_CLASS(qevt);
    TestEventState *s = qevtk->get_user_data(qevt);
    int i;
    bool needs_dispatch = false;

    if (!s->skip_poll) {
        for (i = 0; i < s->n_poll_fds; i++) {
            if (s->poll_fds[i].revents & s->poll_fds[i].events) {
                needs_dispatch = true;
            }
        }
    }

    return needs_dispatch;
}

static bool test_event_dispatch(QSource *evt)
{
    QSourceClass *evtk = QSOURCE_GET_CLASS(evt);
    QSourceCB cb = evtk->get_callback_func(evt);

    if (cb) {
        return cb(evt);
    }

    return true;
}

static void test_event_finalize(QSource *qsource)
{
}

static bool test_cb(QSource *evt)
{
    QSourceClass *evtk = QSOURCE_GET_CLASS(evt);
    TestEventState *s = evtk->get_user_data(evt);

    s->cb_value = CB_VALUE_PASS;
    s->dispatched = true;

    if (!s->skip_poll) {
        int i;
        for (i = 0; i < s->n_poll_fds; i++) {
            /* unless we short-circuited execution, we should've
             * only dispatched if the corresponding events we're
             * listening for were set in the poll() call
             */
            if (!(s->poll_fds[i].revents & s->poll_fds[i].events)) {
                s->cb_value = CB_VALUE_FAIL;
            }
        }
    }

    return true;
}

QSourceFuncs test_funcs = {
    test_event_prepare,
    test_event_check,
    test_event_dispatch,
    test_event_finalize,
};

static void test_qcontext_init(void)
{
    Error *err = NULL;
    GlibQContext *ctx;

    ctx = glib_qcontext_new("test", false, &err);
    g_assert(!err);

    object_unref(OBJECT(ctx));
}

static void test_qsource_init(void)
{
    QSource *event = qsource_new(test_funcs, test_cb, NULL, NULL);

    object_unref(OBJECT(event));
}

static void test_qcontext_attach(void)
{
    GlibQContext *ctx;
    QContextClass *ctxk;
    QSource *evt;
    Error *err = NULL;

    ctx = glib_qcontext_new("test2", false, &err);
    if (err) {
        g_warning("error: %s", error_get_pretty(err));
        g_assert(0);
    }
    ctxk = QCONTEXT_GET_CLASS(QCONTEXT(ctx));
    evt = qsource_new(test_funcs, test_cb, NULL, NULL);

    ctxk->attach(QCONTEXT(ctx), evt, NULL);
    ctxk->detach(QCONTEXT(ctx), evt, NULL);

    object_unref(OBJECT(evt));
    object_unref(OBJECT(ctx));
}

static void test_qcontext_iterate(void)
{
    GlibQContext *ctx;
    QContextClass *ctxk;
    QSource *evt;
    QSourceClass *evtk;
    Error *err = NULL;
    EventNotifier notifier1, notifier2;
    TestEventState s = { 0 };

    /* TODO: generalize this test case to act on any QContext
     * sub-class so we can re-use it for non-glib implementations
     */
    ctx = glib_qcontext_new("test3", false, &err);
    if (err) {
        g_warning("error: %s", error_get_pretty(err));
        g_assert(0);
    }
    ctxk = QCONTEXT_GET_CLASS(QCONTEXT(ctx));

    /* test first iteration. glib uses an internal GPollFD to
     * trigger wake-up when GSources/GPollFDs are added to a
     * context, so poll may return true even before we add
     * QSources to the associated GlibQContext. Since this is
     * an implementation detail of glib we don't explicitly
     * test for poll() return value here, just the other
     * interfaces
     */
    g_assert(ctxk->prepare(QCONTEXT(ctx), NULL) == false);
    ctxk->poll(QCONTEXT(ctx), 0);
    g_assert(ctxk->check(QCONTEXT(ctx)) == false);
    ctxk->dispatch(QCONTEXT(ctx));

    /* attach some events to the context and initialize
     * test state to probe callback behavior
     */
    event_notifier_init(&notifier1, false);
    event_notifier_init(&notifier2, false);
    s.poll_fds[0].fd = event_notifier_get_fd(&notifier1);
    s.poll_fds[1].fd = event_notifier_get_fd(&notifier2);
    s.poll_fds[0].events = s.poll_fds[1].events = G_IO_IN;
    s.n_poll_fds = 2;
    s.skip_poll = false;
    s.dispatched = false;

    evt = qsource_new(test_funcs, test_cb, NULL, &s);
    evtk = QSOURCE_GET_CLASS(evt);
    evtk->add_poll(evt, &s.poll_fds[0]);
    evtk->add_poll(evt, &s.poll_fds[1]);
    ctxk->attach(QCONTEXT(ctx), evt, NULL);

    /* looping with events attached, but no GPollFD events set */
    g_assert(ctxk->prepare(QCONTEXT(ctx), NULL) == false);
    /* poll() should return true when we add events to a QContext */
    g_assert(ctxk->poll(QCONTEXT(ctx), 0) == true);
    g_assert(ctxk->check(QCONTEXT(ctx)) == false);
    ctxk->dispatch(QCONTEXT(ctx));
    /* no events, so no callbacks should have been dispatched for
     * our GPollFDs
     */
    g_assert(!s.dispatched);

    /* try again with some G_IO_IN events set */
    event_notifier_set(&notifier1);
    event_notifier_set(&notifier2);
    s.dispatched = false;

    g_assert(ctxk->prepare(QCONTEXT(ctx), NULL) == false);
    g_assert(ctxk->poll(QCONTEXT(ctx), 0) == true);
    g_assert(ctxk->check(QCONTEXT(ctx)) == true);
    ctxk->dispatch(QCONTEXT(ctx));
    g_assert(s.dispatched);

    s.dispatched = false;

    /* try again with events cleared */
    event_notifier_test_and_clear(&notifier1);
    event_notifier_test_and_clear(&notifier2);

    g_assert(ctxk->prepare(QCONTEXT(ctx), NULL) == false);
    g_assert(ctxk->poll(QCONTEXT(ctx), 0) == true);
    g_assert(ctxk->check(QCONTEXT(ctx)) == false);
    ctxk->dispatch(QCONTEXT(ctx));
    g_assert(!s.dispatched);

    /* try again with short-circuited dispatch */
    s.skip_poll = true;

    g_assert(ctxk->prepare(QCONTEXT(ctx), NULL) == true);
    g_assert(ctxk->poll(QCONTEXT(ctx), 0) == false);
    g_assert(ctxk->check(QCONTEXT(ctx)) == true);
    ctxk->dispatch(QCONTEXT(ctx));
    g_assert(s.dispatched);
    g_assert(s.cb_value == CB_VALUE_PASS);
    s.skip_poll = false;

    s.dispatched = false;

    /* again with all QSources removed */
    ctxk->detach(QCONTEXT(ctx), evt, NULL);

    g_assert(ctxk->prepare(QCONTEXT(ctx), NULL) == false);
    g_assert(ctxk->poll(QCONTEXT(ctx), 0) == true);
    g_assert(ctxk->check(QCONTEXT(ctx)) == false);
    ctxk->dispatch(QCONTEXT(ctx));
    g_assert(!s.dispatched);

    /* cleanup */
    evtk->remove_poll(evt, &s.poll_fds[0]);
    evtk->remove_poll(evt, &s.poll_fds[1]);

    object_unref(OBJECT(evt));
    object_unref(OBJECT(ctx));
}
#endif

static void test_qcontext_init(void)
{
    QContext *ctx = qcontext_new("test-ctx1", false, NULL);
    g_assert(ctx);
}

int main(int argc, char **argv)
{
    module_call_init(MODULE_INIT_QOM);

    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/qcontext/init", test_qcontext_init);
#if 0
    g_test_add_func("/qsource/init", test_qsource_init);
    g_test_add_func("/qcontext/attach", test_qcontext_attach);
    g_test_add_func("/qcontext/iterate", test_qcontext_iterate);
#endif

    return g_test_run();
}
