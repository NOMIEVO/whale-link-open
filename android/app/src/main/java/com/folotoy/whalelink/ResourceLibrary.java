package com.folotoy.whalelink;

import android.content.ContentUris;
import android.content.Context;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.provider.MediaStore;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * 手机侧资源库：把相册照片按需转成设备能直接显示的像素流。
 *
 * <h3>为什么资源库在手机、设备只做缓存</h3>
 * 设备 Flash 一共 8MB，能划给资源的只有 4 个 1MB 槽位；蓝牙实测 25KB/s，
 * 传 24MB 要 16 分钟。所以「把整库同步进设备」在物理上就不成立 ——
 * 手机才是资源库（容量不受限），设备只保留最近用到的几张当缓存窗口。
 *
 * <h3>为什么在手机端缩放</h3>
 * 设备屏幕 240×320，而手机原图动辄 4000×3000。把原图传过去毫无意义：
 *   * 3MB 的 JPEG 空口要 2 分钟，设备还得有 JPEG 解码器（固件里没有）；
 *   * 缩放后 120×160 的 RGB565 只有 38KB，1.5 秒传完。
 * 手机有充足的 CPU 和内存做这件事，设备只有 33KB 堆 —— 分工必须这么切。
 *
 * <h3>落盘格式</h3>
 * 12 字节头 + 紧密排列的 RGB565 小端像素（见固件 whale_ble.h 的
 * WHALE_RES_MAGIC）。设备收到后整块 mmap 给 LVGL，不做任何内存拷贝 ——
 * 240×320 就是 150KB，拷进 33KB 的堆必然失败。
 */
public final class ResourceLibrary {

    private static final String TAG = "ResLib";

    /** 与固件 WHALE_RES_MAGIC 一致："WHLP" 小端读作 0x504C4857。 */
    public static final int RES_MAGIC = 0x504C4857;
    public static final int RES_HDR_LEN = 12;

    /** 传输尺寸上限：120×160 的 RGB565 = 38400 字节，1.5 秒传完。 */
    private static final int TARGET_W = 120;
    private static final int TARGET_H = 160;

    /** 一个可供给的资源。 */
    public static final class Item {
        public final long id;
        public final String name;
        public final Uri uri;

        Item(long id, String name, Uri uri) {
            this.id = id;
            this.name = name;
            this.uri = uri;
        }
    }

    private final Context ctx;
    private List<Item> items = new ArrayList<>();

    public ResourceLibrary(Context ctx) {
        this.ctx = ctx.getApplicationContext();
    }

    /**
     * 扫一遍相册，按拍摄时间倒序（最新的在前）。
     *
     * 只取 id/名字，不读图 —— 几千张照片全解码会直接 OOM，
     * 真正解码只发生在设备请求某一张时。
     *
     * @return &gt;=0 张数；-1 查询出错；-2 没有相册权限（调用方要能区分，
     *         否则「没权限」和「相册是空的」会显示成同一句话，排查方向完全跑偏）
     */
    public int refresh() {
        if (!hasReadPermission()) {
            Log.w(TAG, "没有相册读权限，先申请再扫");
            return -2;
        }
        List<Item> list = new ArrayList<>();
        // VOLUME_EXTERNAL 是「所有外部卷」，能带出 SD 卡里的照片；
        // EXTERNAL_CONTENT_URI 在某些 ROM 上只覆盖主存储。API 29 才有这个常量。
        Uri base = MediaStore.Images.Media.EXTERNAL_CONTENT_URI;
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.Q) {
            base = MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL);
        }
        String[] proj = {
                MediaStore.Images.Media._ID,
                MediaStore.Images.Media.DISPLAY_NAME,
        };
        try (Cursor c = ctx.getContentResolver().query(
                base, proj, null, null,
                MediaStore.Images.Media.DATE_ADDED + " DESC")) {
            if (c != null) {
                final int idCol = c.getColumnIndexOrThrow(MediaStore.Images.Media._ID);
                final int nameCol = c.getColumnIndexOrThrow(
                        MediaStore.Images.Media.DISPLAY_NAME);
                while (c.moveToNext()) {
                    long id = c.getLong(idCol);
                    String name = c.getString(nameCol);
                    Uri uri = ContentUris.withAppendedId(base, id);
                    list.add(new Item(id, name != null ? name : ("img" + id), uri));
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "扫相册失败：" + e);
            return -1;
        }
        items = list;
        Log.i(TAG, "相册共 " + list.size() + " 张");
        return list.size();
    }

    /**
     * 相机胶卷读权限。
     *
     * Android 13 起 READ_EXTERNAL_STORAGE 对图片已失效，必须 READ_MEDIA_IMAGES；
     * Android 14 起用户可选「仅部分照片」，那时只给 READ_MEDIA_VISUAL_USER_SELECTED。
     * 两种都算可用 —— 后者虽然只看到用户挑的那几张，但确实是能读的。
     */
    private boolean hasReadPermission() {
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.TIRAMISU) {
            if (check("android.permission.READ_MEDIA_IMAGES")) {
                return true;
            }
            return android.os.Build.VERSION.SDK_INT >= 34
                    && check("android.permission.READ_MEDIA_VISUAL_USER_SELECTED");
        }
        return check(android.Manifest.permission.READ_EXTERNAL_STORAGE);
    }

    private boolean check(String perm) {
        return androidx.core.content.ContextCompat.checkSelfPermission(ctx, perm)
                == android.content.pm.PackageManager.PERMISSION_GRANTED;
    }

    public int size() {
        return items.size();
    }

    /** 序号按数量取模，翻到末尾自动回到第一张 —— 设备端只管自增。 */
    public Item get(int index) {
        if (items.isEmpty()) {
            return null;
        }
        int i = index % items.size();
        if (i < 0) i += items.size();
        return items.get(i);
    }

    /**
     * 直接把一个 Uri 转成资源包。
     *
     * 给「选择文件并发送」用：用户在系统文件选择器里挑的照片，走的也是这套
     * 转码流程，不需要相册权限（SAF 授权本身即可读），因此它同时是
     * 相册权限拿不到时的兜底路径。
     */
    public byte[] build(Uri uri) {
        if (uri == null) {
            return null;
        }
        return build(new Item(0L, String.valueOf(uri), uri));
    }

    /**
     * 把一张照片转成设备可显示的完整资源包（12B 头 + RGB565 像素）。
     *
     * 长边按比例缩到 120×160 框内，不裁切 —— 照片比例千奇百怪，裁切会
     * 切掉主体，等比缩放最多留两条黑边，视觉上更容易接受。
     *
     * @return 资源包字节；失败返回 null（调用方要能处理"这张读不出来"）
     */
    public byte[] build(Item item) {
        if (item == null) {
            return null;
        }
        Bitmap src = null;
        Bitmap scaled = null;
        try {
            // 先只读尺寸，按 inSampleSize 粗降采样，避免把 4000×3000 整张读进内存
            BitmapFactory.Options probe = new BitmapFactory.Options();
            probe.inJustDecodeBounds = true;
            try (java.io.InputStream in = ctx.getContentResolver().openInputStream(item.uri)) {
                BitmapFactory.decodeStream(in, null, probe);
            }
            if (probe.outWidth <= 0 || probe.outHeight <= 0) {
                Log.w(TAG, "读不出尺寸：" + item.name);
                return null;
            }

            int sample = 1;
            while (probe.outWidth / (sample * 2) >= TARGET_W
                    && probe.outHeight / (sample * 2) >= TARGET_H) {
                sample *= 2;
            }

            BitmapFactory.Options opt = new BitmapFactory.Options();
            opt.inSampleSize = sample;
            opt.inPreferredConfig = Bitmap.Config.ARGB_8888;
            try (java.io.InputStream in = ctx.getContentResolver().openInputStream(item.uri)) {
                src = BitmapFactory.decodeStream(in, null, opt);
            }
            if (src == null) {
                Log.w(TAG, "解码失败：" + item.name);
                return null;
            }

            // 等比缩放到目标框内（不裁切）
            final int sw = src.getWidth();
            final int sh = src.getHeight();
            final float ratio = Math.min((float) TARGET_W / sw, (float) TARGET_H / sh);
            int dw = Math.max(1, Math.round(sw * ratio));
            int dh = Math.max(1, Math.round(sh * ratio));
            scaled = Bitmap.createScaledBitmap(src, dw, dh, true);

            final int pxCount = dw * dh;
            int[] argb = new int[pxCount];
            scaled.getPixels(argb, 0, dw, 0, 0, dw, dh);

            final int dataLen = pxCount * 2;
            byte[] out = new byte[RES_HDR_LEN + dataLen];
            putLe32(out, 0, RES_MAGIC);
            out[4] = (byte) (dw & 0xFF);
            out[5] = (byte) ((dw >> 8) & 0xFF);
            out[6] = (byte) (dh & 0xFF);
            out[7] = (byte) ((dh >> 8) & 0xFF);
            out[8] = 0;                      // fmt = 0 → RGB565 小端
            out[9] = 0;
            out[10] = 0;
            out[11] = 0;

            int o = RES_HDR_LEN;
            for (int i = 0; i < pxCount; i++) {
                short v = RleCodec.toRgb565(argb[i]);
                out[o++] = (byte) (v & 0xFF);
                out[o++] = (byte) ((v >> 8) & 0xFF);
            }
            Log.i(TAG, "转码 " + item.name + " " + sw + "x" + sh + " → " + dw + "x" + dh
                    + "，" + out.length + " 字节");
            return out;
        } catch (Exception e) {
            Log.w(TAG, "转码失败 " + item.name + "：" + e);
            return null;
        } finally {
            if (scaled != null && scaled != src) {
                scaled.recycle();
            }
            if (src != null) {
                src.recycle();
            }
        }
    }

    private static void putLe32(byte[] b, int off, int v) {
        b[off] = (byte) (v & 0xFF);
        b[off + 1] = (byte) ((v >> 8) & 0xFF);
        b[off + 2] = (byte) ((v >> 16) & 0xFF);
        b[off + 3] = (byte) ((v >> 24) & 0xFF);
    }
}
