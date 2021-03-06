#include "uxenv4vlib_private.h"


typedef struct {
    LIST_ENTRY le;
    v4v_addr_t dst;
    uxen_v4v_callback_t *callback;
    void *callback_data1;
    void *callback_data2;
    uint32_t len;
    uint32_t triggered;
} uxen_v4v_notify_t;


BOOLEAN
uxen_v4v_notify_dequeue (v4v_addr_t *dst,
                         uxen_v4v_callback_t *callback, void *callback_data1,
                         void *callback_data2)
{
    uxen_v4v_notify_t *p;
    xenv4v_extension_t *pde;
    KIRQL irql;
    int found = 0;
    BOOLEAN ret = FALSE;


    pde = uxen_v4v_get_pde ();

    if (!pde)
        return ret;

    KeAcquireSpinLock (&pde->queue_lock, &irql);

    do {
        found = 0;


        //XXX: fix these to traverse lists only once, windows must have a macro for that

        for (p = (uxen_v4v_notify_t *) pde->notify_list.Flink;
             p != (uxen_v4v_notify_t *) & pde->notify_list;
             p = (uxen_v4v_notify_t *) p->le.Flink) {
            if (p->dst.port != dst->port)
                continue;
            if (p->dst.domain != dst->domain)
                continue;
            if (p->callback != callback)
                continue;
            if (p->callback_data1 != callback_data1)
                continue;
            if (p->callback_data2 != callback_data2)
                continue;

            found++;
            break;
        }

        if (!found)
            break;

        RemoveEntryList (&p->le);
        uxen_v4v_fast_free (p);

        ret = TRUE;

    } while (found);

    KeReleaseSpinLock (&pde->queue_lock, irql);

    uxen_v4v_put_pde (pde);

    return ret;
}


void
uxen_v4v_notify_enqueue (uint32_t len, v4v_addr_t *dst,
                         uxen_v4v_callback_t *callback, void *callback_data1,
                         void *callback_data2)
{
    uxen_v4v_notify_t *n, *p;
    xenv4v_extension_t *pde;
    KIRQL irql;
    int found = 0;

    pde = uxen_v4v_get_pde ();

    if (!pde)
        return;

    n = (uxen_v4v_notify_t *) uxen_v4v_fast_alloc(sizeof (uxen_v4v_notify_t));
    if (!n) {
        uxen_v4v_put_pde (pde);
        uxen_v4v_err("allocation of notification failed");
        return;
    }

    RtlZeroMemory (n, sizeof (uxen_v4v_notify_t));
    InitializeListHead (&n->le);

    memcpy (&n->dst, dst, sizeof (n->dst));
    n->len = len;
    n->callback = callback;
    n->callback_data1 = callback_data1;
    n->callback_data2 = callback_data2;
    n->triggered = 0;

    KeAcquireSpinLock (&pde->queue_lock, &irql);

    do {

        //XXX: fix these to traverse lists only once, windows must have a macro for that
        for (p = (uxen_v4v_notify_t *) pde->notify_list.Flink;
             p != (uxen_v4v_notify_t *) & pde->notify_list;
             p = (uxen_v4v_notify_t *) p->le.Flink) {
            if (p->dst.port != n->dst.port)
                continue;
            if (p->dst.domain != n->dst.domain)
                continue;
            if (p->callback != n->callback)
                continue;
            if (p->callback_data1 != n->callback_data1)
                continue;
            if (p->callback_data2 != n->callback_data2)
                continue;

            p->len = n->len;

            p->triggered = 0;

            found++;
        }

        if (found)
            break;

        InsertTailList (&pde->notify_list, &n->le);

    } while (0);

    KeReleaseSpinLock (&pde->queue_lock, irql);

    if (found)
        uxen_v4v_fast_free (n);

    uxen_v4v_put_pde (pde);

}

/* caller to hold queue lock, answer only valid for duration of lock */
unsigned int
uxen_v4v_notify_count (xenv4v_extension_t *pde)
{
    unsigned int ret = 0;
    uxen_v4v_notify_t *p;

    if (!pde)
        return ret;

    for (p = (uxen_v4v_notify_t *) pde->notify_list.Flink;
         p != (uxen_v4v_notify_t *) & pde->notify_list;
         p = (uxen_v4v_notify_t *) p->le.Flink) {
        if (p->triggered)
            continue;
        ret++;
    }



    return ret;
}



/* caller to hold queue lock, answer only valid for duration of lock */
unsigned int
uxen_v4v_notify_fill_ring_data (xenv4v_extension_t *pde,
                                v4v_ring_data_ent_t *ring_data,
                                unsigned int count)
{
    unsigned int i = 0;
    uxen_v4v_notify_t *p;

    for (p = (uxen_v4v_notify_t *) pde->notify_list.Flink;
         p != (uxen_v4v_notify_t *) & pde->notify_list;
         p = (uxen_v4v_notify_t *) p->le.Flink) {
        if (p->triggered)
            continue;
        if (i >= count)
            continue;


        ring_data[i].ring = p->dst;
        ring_data[i].space_required = p->len;


        i++;
    }


    return i;

}



void
uxen_v4v_notify_process_ring_data (xenv4v_extension_t *pde,
                                   v4v_ring_data_ent_t *ring_data,
                                   unsigned int count)
{
    KIRQL irql;
    unsigned int i;
    unsigned int run_notify = 0;
    uxen_v4v_notify_t *p;

    KeAcquireSpinLock (&pde->queue_lock, &irql);


    for (p = (uxen_v4v_notify_t *) pde->notify_list.Flink;
         p != (uxen_v4v_notify_t *) & pde->notify_list;
         p = (uxen_v4v_notify_t *) p->le.Flink) {

        for (i = 0; i < count; ++i) {
            if (!(ring_data[i].flags & V4V_RING_DATA_F_SUFFICIENT))
                continue;
            if (ring_data[i].ring.port != p->dst.port)
                continue;
            if (ring_data[i].ring.domain != p->dst.domain)
                continue;
            p->triggered = 1;
            run_notify = 1;
        }
    }


    KeReleaseSpinLock (&pde->queue_lock, irql);

    if (run_notify)
        KeSetEvent(&pde->notify_event, IO_NO_INCREMENT, FALSE);

}

static void
uxen_v4v_notify_work(xenv4v_extension_t *pde)
{
    uxen_v4v_notify_t *p, *n;
    KIRQL irql;

    KeRaiseIrql(DISPATCH_LEVEL, &irql);
    for (;;) {

        n = NULL;

        KeAcquireSpinLockAtDpcLevel(&pde->queue_lock);

        for (p = (uxen_v4v_notify_t *) pde->notify_list.Flink;
             p != (uxen_v4v_notify_t *) & pde->notify_list;
             p = (uxen_v4v_notify_t *) p->le.Flink) {
            if (p->triggered) {
                n = p;
                RemoveEntryList (&n->le);
                break;
            }
        }


        KeReleaseSpinLockFromDpcLevel(&pde->queue_lock);

        if (!n)
            break;


        n->callback (NULL, n->callback_data1, n->callback_data2);
        uxen_v4v_fast_free (n);
    }
    KeLowerIrql(irql);
}

void
uxen_v4v_notify_thread(void *context)
{
    xenv4v_extension_t *pde =
        v4v_get_device_extension((DEVICE_OBJECT *)context);

    while (InterlockedExchangeAdd(&pde->notify_thread_running, 0)) {
        KeWaitForSingleObject(&pde->notify_event, Executive, KernelMode, TRUE,
                              NULL);
        KeClearEvent(&pde->notify_event);
        if (!InterlockedExchangeAdd(&pde->notify_thread_running, 0))
            break;

        uxen_v4v_notify_work(pde);
    }
}
