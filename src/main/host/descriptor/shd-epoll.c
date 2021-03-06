/*
 * The Shadow Simulator
 * Copyright (c) 2010-2011, Rob Jansen
 * See LICENSE for licensing information
 */

#include <unistd.h>
#include "shadow.h"

typedef enum _EpollWatchFlags EpollWatchFlags;
enum _EpollWatchFlags {
    EWF_NONE = 0,
    /* the underlying shadow descriptor is initialized and operational */
    EWF_ACTIVE = 1 << 0,
    /* the underlying shadow descriptor is readable */
    EWF_READABLE = 1 << 1,
    /* the application is waiting for a read event on the underlying shadow descriptor */
    EWF_WAITINGREAD = 1 << 2,
    /* the readable status changed but the event has not yet been collected (for EDGETRIGGER) */
    EWF_READCHANGED = 1 << 3,
    /* the underlying shadow descriptor is writable */
    EWF_WRITEABLE = 1 << 4,
    /* the application is waiting for a write event on the underlying shadow descriptor */
    EWF_WAITINGWRITE = 1 << 5,
    /* the writable status changed but the event has not yet been collected (for EDGETRIGGER) */
    EWF_WRITECHANGED = 1 << 6,
    /* the underlying shadow descriptor is closed */
    EWF_CLOSED = 1 << 7,
    /* true if this watch is currently valid and in the watches table. this allows
     * support of lazy deletion of watches that are in the reportable queue when
     * we want to delete them, to avoid the O(n) removal time of the queue. */
    EWF_WATCHING = 1 << 8,
    /* set if edge-triggered events are enabled on the underlying shadow descriptor */
    EWF_EDGETRIGGER = 1 << 9,
    EWF_EDGETRIGGER_REPORTED = 1 << 10,
    /* set if one-shot events are enabled on the underlying shadow descriptor */
    EWF_ONESHOT = 1 << 11,
    /* used to track that ONESHOT mode is used, an event was already reported, and the
     * socket has not been modified since. This prevents duplicate reporting in ONESHOT mode. */
    EWF_ONESHOT_REPORTED = 1 << 12,
};

typedef struct _EpollWatch EpollWatch;
struct _EpollWatch {
    /* the shadow descriptor we are watching for events */
    Descriptor* descriptor;
    /* holds the actual event info */
    struct epoll_event event;
    /* current status of the underlying shadow descriptor */
    EpollWatchFlags flags;
    gint referenceCount;
    MAGIC_DECLARE;
};

typedef enum _EpollFlags EpollFlags;
enum _EpollFlags {
    EF_NONE = 0,
    /* a callback is currently scheduled to notify user
     * (used to avoid duplicate notifications) */
    EF_SCHEDULED = 1 << 0,
    /* we are currently notifying the process of events on its watched descriptors */
    EF_NOTIFYING = 1 << 1,
    /* the plugin closed the epoll descriptor, we should close as
     * soon as the notify is no longer scheduled */
    EF_CLOSED = 1 << 2,
};

struct _Epoll {
    /* epoll itself is also a descriptor */
    Descriptor super;

    /* other members specific to epoll */
    EpollFlags flags;

    /* holds the wrappers for the descriptors we are watching for events */
    GHashTable* watching;

    Process* ownerProcess;
    gint osEpollDescriptor;

    MAGIC_DECLARE;
};

/* forward declaration */
static void _epoll_tryNotify(Epoll* epoll, gpointer userData);

static EpollWatch* _epollwatch_new(Epoll* epoll, Descriptor* descriptor, struct epoll_event* event) {
    EpollWatch* watch = g_new0(EpollWatch, 1);
    MAGIC_INIT(watch);
    utility_assert(event);

    /* ref it for the EpollWatch, which also covers the listener reference
     * (which is freed below in _epollwatch_free) */
    descriptor_ref(descriptor);

    watch->descriptor = descriptor;
    watch->event = *event;
    watch->referenceCount = 1;

    return watch;
}

static void _epollwatch_free(EpollWatch* watch) {
    MAGIC_ASSERT(watch);

    descriptor_unref(watch->descriptor);

    MAGIC_CLEAR(watch);
    g_free(watch);
}

static void _epollwatch_ref(EpollWatch* watch) {
    MAGIC_ASSERT(watch);
    (watch->referenceCount)++;
}

static void _epollwatch_unref(EpollWatch* watch) {
    MAGIC_ASSERT(watch);

    if(--(watch->referenceCount) <= 0) {
        _epollwatch_free(watch);
    }
}

/* should only be called from descriptor dereferencing the functionTable */
static void _epoll_free(Epoll* epoll) {
    MAGIC_ASSERT(epoll);

    /* this unrefs all of the remaining watches */
    g_hash_table_destroy(epoll->watching);

    close(epoll->osEpollDescriptor);

    utility_assert(epoll->ownerProcess);
    process_unref(epoll->ownerProcess);

    MAGIC_CLEAR(epoll);
    g_free(epoll);

    worker_countObject(OBJECT_TYPE_EPOLL, COUNTER_TYPE_FREE);
}

void epoll_clearWatchListeners(Epoll* epoll) {
    MAGIC_ASSERT(epoll);

    /* make sure none of our watch descriptors notify us anymore */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, epoll->watching);
    while(g_hash_table_iter_next(&iter, &key, &value)) {
        EpollWatch* watch = value;
        MAGIC_ASSERT(watch);
        descriptor_removeEpollListener(watch->descriptor, (Descriptor*)epoll);
    }
}

static void _epoll_close(Epoll* epoll) {
    MAGIC_ASSERT(epoll);

    epoll_clearWatchListeners(epoll);

    /* tell the host to stop tracking us, and unref the descriptor.
     * this should trigger _epoll_free in most cases. */
    host_closeDescriptor(worker_getActiveHost(), epoll->super.handle);
}

static void _epoll_tryToClose(Epoll* epoll) {
    MAGIC_ASSERT(epoll);

    /* mark the descriptor as closed */
    epoll->flags |= EF_CLOSED;

    /* only close it if there is no pending epoll notify event */
    gboolean isScheduled = (epoll->flags & EF_SCHEDULED) ? TRUE : FALSE;
    if(!isScheduled) {
        _epoll_close(epoll);
    }
}

DescriptorFunctionTable epollFunctions = {
    (DescriptorFunc) _epoll_tryToClose,
    (DescriptorFunc) _epoll_free,
    MAGIC_VALUE
};

Epoll* epoll_new(gint handle) {
    Epoll* epoll = g_new0(Epoll, 1);
    MAGIC_INIT(epoll);

    descriptor_init(&(epoll->super), DT_EPOLL, &epollFunctions, handle);

    /* allocate backend needed for managing events for this descriptor */
    epoll->watching = g_hash_table_new_full(g_int_hash, g_int_equal, NULL, (GDestroyNotify)_epollwatch_unref);

    /* the application may want us to watch some system files, so we need a
     * real OS epoll fd so we can offload that task.
     */
    epoll->osEpollDescriptor = epoll_create(1000);
    if(epoll->osEpollDescriptor == -1) {
        warning("error in epoll_create for OS events, errno=%i msg:%s", errno, g_strerror(errno));
    }

    /* keep track of which virtual application we need to notify of events
    epoll_new should be called as a result of an application syscall */
    epoll->ownerProcess = worker_getActiveProcess();
    utility_assert(epoll->ownerProcess);
    process_ref(epoll->ownerProcess);

    /* the epoll descriptor itself is always able to be epolled */
    descriptor_adjustStatus(&(epoll->super), DS_ACTIVE, TRUE);

    worker_countObject(OBJECT_TYPE_EPOLL, COUNTER_TYPE_NEW);

    return epoll;
}

static void _epollwatch_updateStatus(EpollWatch* watch) {
    MAGIC_ASSERT(watch);

    /* store the old flags that are only lazily updated */
    EpollWatchFlags lazyFlags = 0;
    lazyFlags |= (watch->flags & EWF_READCHANGED) ? EWF_READCHANGED : EWF_NONE;
    lazyFlags |= (watch->flags & EWF_WRITECHANGED) ? EWF_WRITECHANGED : EWF_NONE;
    lazyFlags |= (watch->flags & EWF_WATCHING) ? EWF_WATCHING : EWF_NONE;
    lazyFlags |= (watch->flags & EWF_EDGETRIGGER_REPORTED) ? EWF_EDGETRIGGER_REPORTED : EWF_NONE;
    lazyFlags |= (watch->flags & EWF_ONESHOT_REPORTED) ? EWF_ONESHOT_REPORTED : EWF_NONE;

    /* reset our flags */
    EpollWatchFlags oldFlags = watch->flags;
    watch->flags = 0;

    /* check shadow descriptor status */
    DescriptorStatus status = descriptor_getStatus(watch->descriptor);
    watch->flags |= (status & DS_ACTIVE) ? EWF_ACTIVE : EWF_NONE;
    watch->flags |= (status & DS_READABLE) ? EWF_READABLE : EWF_NONE;
    watch->flags |= (status & DS_WRITABLE) ? EWF_WRITEABLE : EWF_NONE;
    watch->flags |= (status & DS_CLOSED) ? EWF_CLOSED : EWF_NONE;
    watch->flags |= (watch->event.events & EPOLLIN) ? EWF_WAITINGREAD : EWF_NONE;
    watch->flags |= (watch->event.events & EPOLLOUT) ? EWF_WAITINGWRITE : EWF_NONE;
    watch->flags |= (watch->event.events & EPOLLET) ? EWF_EDGETRIGGER : EWF_NONE;
    watch->flags |= (watch->event.events & EPOLLONESHOT) ? EWF_ONESHOT : EWF_NONE;

    /* add back in our lazyFlags that we dont check separately */
    watch->flags |= lazyFlags;

    /* update changed status for edgetrigger mode */
    if((oldFlags & EWF_READABLE) != (watch->flags & EWF_READABLE)) {
        watch->flags |= EWF_READCHANGED;
    }
    if((oldFlags & EWF_WRITEABLE) != (watch->flags & EWF_WRITEABLE)) {
        watch->flags |= EWF_WRITECHANGED;
    }
}

static gboolean _epollwatch_isReady(EpollWatch* watch) {
    MAGIC_ASSERT(watch);

    /* make sure we have the latest info for this watched descriptor */
    _epollwatch_updateStatus(watch);

    /* if its closed, not active, or no parent is watching it, then we are not ready */
    if((watch->flags & EWF_CLOSED) || !(watch->flags & EWF_ACTIVE) || !(watch->flags & EWF_WATCHING)) {
        return FALSE;
    }

    gboolean isReady = FALSE;

    gboolean hasReadEvent = (watch->flags & EWF_READABLE) && (watch->flags & EWF_WAITINGREAD) ? TRUE : FALSE;
    gboolean hasWriteEvent = (watch->flags & EWF_WRITEABLE) && (watch->flags & EWF_WAITINGWRITE) ? TRUE : FALSE;

    /* figure out if we should report an event */
    if(watch->flags & EWF_EDGETRIGGER) {
        /* edge-triggered mode is only ready if the read/write event status changed, unless there is
         * an event and we have yet to report it. */
        if(hasReadEvent && ((watch->flags & EWF_READCHANGED) || !(watch->flags & EWF_EDGETRIGGER_REPORTED))) {
            isReady = TRUE;
        }
        if(hasWriteEvent && ((watch->flags & EWF_WRITECHANGED) || !(watch->flags & EWF_EDGETRIGGER_REPORTED))) {
            isReady = TRUE;
        }
    } else {
        /* default level-triggered mode always reports events that exist */
        if(hasReadEvent || hasWriteEvent) {
            isReady =  TRUE;
        }
    }

    /* ONESHOT mode only reports once until a change happens */
    if(isReady && (watch->flags & EWF_ONESHOT) && (watch->flags & EWF_ONESHOT_REPORTED)) {
        isReady = FALSE;
    }

    return isReady;
}

static gboolean _epoll_isReadyOS(Epoll* epoll) {
    MAGIC_ASSERT(epoll);
    gboolean isReady = FALSE;

    if(epoll->osEpollDescriptor >= 3) {
        /* the os epoll will be readable when ready */
        struct epoll_event epoll_ev;
        memset(&epoll_ev, 0, sizeof(struct epoll_event));
        epoll_ev.events = EPOLLIN;

        /* create a temp epoll to check if our OS epoll is ready */
        gint readinessFD = epoll_create(1);
        gint ret = epoll_ctl(readinessFD, EPOLL_CTL_ADD, epoll->osEpollDescriptor, &epoll_ev);
        if(ret == 0) {
            /* try to collect an event without blocking */
            ret = epoll_wait(readinessFD, &epoll_ev, 1, 0);
            if(ret > 0) {
                /* osEpollDescriptor has an EPOLLIN event, so it has events we should collect */
                isReady = TRUE;
            }

            /* cleanup */
            epoll_ctl(readinessFD, EPOLL_CTL_DEL, epoll->osEpollDescriptor, NULL);
        }
        close(readinessFD);
    }

    return isReady;
}

static void _epoll_check(Epoll* epoll) {
    MAGIC_ASSERT(epoll);

    /* if we are currently here because epoll called process_continue,
     * then just skip out; after we return from process_continue, we'll
     * do another check then (which will pass this check) */
    if((epoll->flags & EF_CLOSED) || (epoll->flags & EF_NOTIFYING)) {
        return;
    }

    /* check status to see if we need to schedule a notification */
    gboolean isReady = FALSE;

    /* check all of our children */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, epoll->watching);
    while(g_hash_table_iter_next(&iter, &key, &value)) {
        EpollWatch* watch = value;
        MAGIC_ASSERT(watch);

        if(_epollwatch_isReady(watch)) {
            isReady = TRUE;
            /* at least one child is ready, stop looping */
            break;
        }
    }

    /* check for events on the OS epoll instance, but only if we are otherwise not ready */
    if(!isReady && _epoll_isReadyOS(epoll)) {
        isReady = TRUE;
    }

    if(isReady) {
        /* some children are ready, so this epoll is readable */
        descriptor_adjustStatus(&(epoll->super), DS_READABLE, TRUE);

        /* schedule a notification event for our node, if wanted and one isnt already scheduled */
        if(!(epoll->flags & EF_SCHEDULED) && process_wantsNotify(epoll->ownerProcess, epoll->super.handle)) {
            descriptor_ref(epoll);
            Task* notifyTask = task_new((TaskCallbackFunc)_epoll_tryNotify,
                    epoll, NULL, descriptor_unref, NULL);

            if(worker_scheduleTask(notifyTask, 1)) {
                epoll->flags |= EF_SCHEDULED;
            }
            task_unref(notifyTask);
        }
    } else {
        descriptor_adjustStatus(&(epoll->super), DS_READABLE, FALSE);
    }
}

static const gchar* _epoll_operationToStr(gint op) {
    switch(op) {
    case EPOLL_CTL_ADD:
        return "EPOLL_CTL_ADD";
    case EPOLL_CTL_DEL:
        return "EPOLL_CTL_DEL";
    case EPOLL_CTL_MOD:
        return "EPOLL_CTL_MOD";
    default:
        return "unknown";
    }
}

gint epoll_control(Epoll* epoll, gint operation, Descriptor* descriptor, struct epoll_event* event) {
    MAGIC_ASSERT(epoll);

    debug("epoll descriptor %i, operation %s, descriptor %i",
            epoll->super.handle, _epoll_operationToStr(operation), descriptor->handle);

    EpollWatch* watch = g_hash_table_lookup(epoll->watching, descriptor_getHandleReference(descriptor));

    switch (operation) {
        case EPOLL_CTL_ADD: {
            /* EEXIST op was EPOLL_CTL_ADD, and the supplied file descriptor
             * fd is already registered with this epoll instance. */
            if(watch) {
                return EEXIST;
            }

            /* start watching for status changes */
            watch = _epollwatch_new(epoll, descriptor, event);
            watch->flags |= EWF_WATCHING;
            g_hash_table_replace(epoll->watching, descriptor_getHandleReference(watch->descriptor), watch);

            /* its added, so we need to listen for changes */
            descriptor_addEpollListener(watch->descriptor, (Descriptor*)epoll);

            /* initiate a callback if the new watched descriptor is ready */
            _epoll_check(epoll);

            break;
        }

        case EPOLL_CTL_MOD: {
            /* ENOENT op was EPOLL_CTL_MOD, and fd is not registered with this epoll instance */
            if(!watch) {
                return ENOENT;
            }

            MAGIC_ASSERT(watch);
            utility_assert(event && (watch->flags & EWF_WATCHING));

            /* the user set new events */
            watch->event = *event;
            /* we would need to report the new event again if in ET or ONESHOT modes */
            watch->flags &= ~EWF_EDGETRIGGER_REPORTED;
            watch->flags &= ~EWF_ONESHOT_REPORTED;

            /* initiate a callback if the new event type on the watched descriptor is ready */
            _epoll_check(epoll);

            break;
        }

        case EPOLL_CTL_DEL: {
            /* ENOENT op was EPOLL_CTL_DEL, and fd is not registered with this epoll instance */
            if(!watch) {
                return ENOENT;
            }

            MAGIC_ASSERT(watch);
            watch->flags &= ~EWF_WATCHING;

            /* its deleted, so stop listening for updates */
            descriptor_removeEpollListener(watch->descriptor, (Descriptor*)epoll);

            /* unref gets called on the watch when it is removed from this table */
            g_hash_table_remove(epoll->watching, descriptor_getHandleReference(watch->descriptor));

            break;
        }

        default: {
            warning("ignoring unrecognized operation");
            break;
        }
    }

    return 0;
}

gint epoll_controlOS(Epoll* epoll, gint operation, gint fileDescriptor,
        struct epoll_event* event) {
    MAGIC_ASSERT(epoll);
    /* ask the OS about any events on our kernel epoll descriptor */
    gint ret = epoll_ctl(epoll->osEpollDescriptor, operation, fileDescriptor, event);
    if(ret < 0) {
        ret = errno;
    }
    return ret;
}

gint epoll_getEvents(Epoll* epoll, struct epoll_event* eventArray, gint eventArrayLength, gint* nEvents) {
    MAGIC_ASSERT(epoll);
    utility_assert(nEvents);

    /* return the available events in the eventArray, making sure not to
     * overflow. the number of actual events is returned in nEvents. */
    gint eventIndex = 0;

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, epoll->watching);
    while(g_hash_table_iter_next(&iter, &key, &value) && (eventIndex < eventArrayLength)) {
        EpollWatch* watch = value;
        MAGIC_ASSERT(watch);

        if(_epollwatch_isReady(watch)) {
            /* report the event */
            eventArray[eventIndex] = watch->event;
            eventArray[eventIndex].events = 0;

            if((watch->flags & EWF_READABLE) && (watch->flags & EWF_WAITINGREAD)) {
                eventArray[eventIndex].events |= EPOLLIN;
            }
            if((watch->flags & EWF_WRITEABLE) && (watch->flags & EWF_WAITINGWRITE)) {
                eventArray[eventIndex].events |= EPOLLOUT;
            }
            if(watch->flags & EWF_EDGETRIGGER) {
                eventArray[eventIndex].events |= EPOLLET;
            }

            /* event was just collected, unset the change status */
            watch->flags &= ~EWF_READCHANGED;
            watch->flags &= ~EWF_WRITECHANGED;

            eventIndex++;
            utility_assert(eventIndex <= eventArrayLength);

            if(watch->flags & EWF_EDGETRIGGER) {
                /* tag that an event was collected in ET mode */
                watch->flags |= EWF_EDGETRIGGER_REPORTED;
            }
            if(watch->flags & EWF_ONESHOT) {
                /* they collected the event, dont report any more */
                watch->flags |= EWF_ONESHOT_REPORTED;
            }
        }
    }

    gint space = eventArrayLength - eventIndex;
    if(space) {
        /* now we have to get events from the OS descriptors */
        struct epoll_event osEvents[space];
        memset(&osEvents, 0, space*sizeof(struct epoll_event));

        /* since we are in shadow context, this will be forwarded to the OS epoll */
        gint nos = epoll_wait(epoll->osEpollDescriptor, osEvents, space, 0);

        if(nos == -1) {
            warning("error in epoll_wait for OS events on epoll fd %i", epoll->osEpollDescriptor);
        }

        /* nos will fit into eventArray */
        for(gint j = 0; j < nos; j++) {
            eventArray[eventIndex] = osEvents[j];
            eventIndex++;
            utility_assert(eventIndex <= eventArrayLength);
        }
    }

    *nEvents = eventIndex;

    debug("epoll descriptor %i collected %i events", epoll->super.handle, eventIndex);

    /* if we consumed all the events that we had to report,
     * then our parent descriptor can no longer read child epolls */
    _epoll_check(epoll);

    return 0;
}

void epoll_descriptorStatusChanged(Epoll* epoll, Descriptor* descriptor) {
    MAGIC_ASSERT(epoll);

    /* make sure we are actually watching the descriptor */
    EpollWatch* watch = g_hash_table_lookup(epoll->watching, descriptor_getHandleReference(descriptor));

    /* if we are not watching, its an error because we shouldn't be listening */
    utility_assert(watch && (watch->descriptor == descriptor));

    debug("status changed in epoll %i for descriptor %i", epoll->super.handle, descriptor->handle);

    /* check the status and take the appropriate action */
    _epoll_check(epoll);
}

#ifdef DEBUG
static gchar* _epoll_getChildrenStatus(Epoll* epoll, GString* message) {
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, epoll->watching);
    while(g_hash_table_iter_next(&iter, &key, &value)) {
        EpollWatch* watch = value;
        MAGIC_ASSERT(watch);

        gboolean isReady = _epollwatch_isReady(watch);
        if(watch->descriptor) {
            g_string_append_printf(message, " %i%s", watch->descriptor->handle, isReady ? "!" : "");
            if(watch->descriptor->type == DT_EPOLL) {
                g_string_append_printf(message, "{");
                _epoll_getChildrenStatus((Epoll*)watch->descriptor, message);
                g_string_append_printf(message, "}");
            }
        }
    }
    return message->str;
}
#endif

static void _epoll_tryNotify(Epoll* epoll, gpointer userData) {
    MAGIC_ASSERT(epoll);

    /* event is being executed from the scheduler, so its no longer scheduled */
    epoll->flags &= ~EF_SCHEDULED;

    /* if it was closed in the meantime, do the actual close now */
    gboolean isClosed = (epoll->flags & EF_CLOSED) ? TRUE : FALSE;
    if(isClosed || !process_isRunning(epoll->ownerProcess)) {
        _epoll_close(epoll);
        return;
    }

    /* we should notify the plugin only if we still have some events to report */
    gboolean isReady = FALSE;
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, epoll->watching);
    while(g_hash_table_iter_next(&iter, &key, &value)) {
        EpollWatch* watch = value;
        MAGIC_ASSERT(watch);

        if(_epollwatch_isReady(watch)) {
            isReady = TRUE;
        }
    }

    /* check if there is events on the OS epoll instance, but only if we would otherwise
     * not call the process. this ensures the process can collect events for which we are
     * using the OS as a backend, even if none of our own watches have ready events. */
    if(!isReady && _epoll_isReadyOS(epoll)) {
        isReady = TRUE;
    }

    if(isReady) {
        /* an event should have only been scheduled for the special epollfd */
        utility_assert(process_wantsNotify(epoll->ownerProcess, epoll->super.handle));

#ifdef DEBUG
        /* debug message for looking at the epoll tree */
        GString* childStatusMessage = g_string_new("");
        gchar* msg = _epoll_getChildrenStatus(epoll, childStatusMessage);
        debug("epollfd %i BEFORE process_continue: child fd statuses:%s", epoll->super.handle, msg);
        g_string_free(childStatusMessage, TRUE);
#endif

        /* notify application to collect the reportable events */
        epoll->flags |= EF_NOTIFYING;
        process_continue(epoll->ownerProcess);
        epoll->flags &= ~EF_NOTIFYING;

#ifdef DEBUG
        /* debug message for looking at the epoll tree */
        childStatusMessage = g_string_new("");
        msg = _epoll_getChildrenStatus(epoll, childStatusMessage);
        debug("epollfd %i AFTER process_continue: child fd statuses:%s", epoll->super.handle, msg);
        g_string_free(childStatusMessage, TRUE);
#endif

        /* set up another shadow callback event if needed */
        _epoll_check(epoll);
    }
}
