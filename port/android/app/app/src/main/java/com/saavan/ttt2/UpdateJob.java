package com.saavan.ttt2;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobService;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Build;

/**
 * A notification when a new version is on volvisionmods.com, also while the app is closed: Android runs this about
 * twice a day when the phone is online (and after a restart). Each new version is announced once.
 */
public final class UpdateJob extends JobService {
    private static final int JOB_ID = 0x7712;
    static final String CHANNEL = "updates";

    /** Called at every launcher start; scheduling again is harmless. */
    static void schedule(Context c) {
        try {
            JobScheduler js = (JobScheduler) c.getSystemService(Context.JOB_SCHEDULER_SERVICE);
            if (js == null) return;
            for (JobInfo j : js.getAllPendingJobs()) if (j.getId() == JOB_ID) return;
            JobInfo job = new JobInfo.Builder(JOB_ID, new ComponentName(c, UpdateJob.class))
                    .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
                    .setPeriodic(12L * 60 * 60 * 1000)
                    .setPersisted(true)
                    .build();
            js.schedule(job);
        } catch (RuntimeException ignored) {
            // No job scheduler access: the launcher's own check still runs.
        }
    }

    static void createChannel(Context c) {
        NotificationManager nm = (NotificationManager) c.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm == null) return;
        NotificationChannel ch = new NotificationChannel(CHANNEL, "Game updates", NotificationManager.IMPORTANCE_DEFAULT);
        ch.setDescription("A new version of Tekken Tag 2 is available on volvisionmods.com");
        nm.createNotificationChannel(ch);
    }

    /** One notification per new version; tapping it opens the game page. */
    static void notifyUpdate(Context c, VolvisionUpdateChannel.Result r) {
        SharedPreferences p = c.getSharedPreferences("ttt2", Context.MODE_PRIVATE);
        if (r.latest.equals(p.getString("update_notified", ""))) return;
        NotificationManager nm = (NotificationManager) c.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm == null) return;
        if (Build.VERSION.SDK_INT >= 33 && !nm.areNotificationsEnabled()) return;
        createChannel(c);
        Intent open = new Intent(Intent.ACTION_VIEW, Uri.parse(r.url));
        open.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        PendingIntent pi = PendingIntent.getActivity(c, 0, open, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Notification n = new Notification.Builder(c, CHANNEL)
                .setSmallIcon(R.mipmap.ic_launcher)
                .setContentTitle("Tekken Tag 2 v" + r.latest + " is available")
                .setContentText(r.notes)
                .setStyle(new Notification.BigTextStyle().bigText(r.notes + "\nTap to open the game page."))
                .setContentIntent(pi)
                .setAutoCancel(true)
                .build();
        try {
            nm.notify(JOB_ID, n);
            p.edit().putString("update_notified", r.latest).apply();
        } catch (SecurityException ignored) {
            // Notifications not allowed.
        }
    }

    @Override
    public boolean onStartJob(final JobParameters params) {
        new Thread(() -> {
            VolvisionUpdateChannel.Result r = VolvisionUpdateChannel.fetch(VolvisionUpdateChannel.versionName(this));
            if (r.reached && r.newer) notifyUpdate(this, r);
            jobFinished(params, !r.reached);
        }, "update-job").start();
        return true;
    }

    @Override
    public boolean onStopJob(JobParameters params) { return true; }
}
