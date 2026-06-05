using Microsoft.UI.Dispatching;
using System;
using System.Runtime;

namespace FernUI
{
    internal static class MemoryCleanup
    {
        public static void CollectNow()
        {
            GCSettings.LargeObjectHeapCompactionMode = GCLargeObjectHeapCompactionMode.CompactOnce;
            GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, blocking: true, compacting: true);
            GC.WaitForPendingFinalizers();
            GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, blocking: true, compacting: true);
        }

        public static void CollectNowAndAfter(DispatcherQueue dispatcherQueue)
        {
            CollectNow();
            ScheduleCollect(dispatcherQueue, TimeSpan.FromMilliseconds(250));
            ScheduleCollect(dispatcherQueue, TimeSpan.FromSeconds(1));
        }

        private static void ScheduleCollect(DispatcherQueue dispatcherQueue, TimeSpan delay)
        {
            DispatcherQueueTimer? timer = dispatcherQueue.CreateTimer();
            timer.Interval = delay;
            timer.Tick += (_, _) =>
            {
                timer.Stop();
                timer = null;
                CollectNow();
            };
            timer.Start();
        }
    }
}
