package com.folotoy.whalelink;

/**
 * RGB565 + RLE。token 首字节：
 *   bit7 = 1 → run，长度 = 低 7 位（1..127），后跟 2 字节 RGB565
 *   bit7 = 0 → raw，个数 = 低 7 位（1..127），后跟 n×2 字节
 *
 * 手机 UI 截图大面积纯色，实测 3–8 倍压缩；照片类内容最坏会涨到 1.5 倍，
 * 调用方要检查结果大小，超限就降分辨率重编。
 */
public final class RleCodec {

    private RleCodec() {
    }

    /** ARGB 像素 → RGB565（不做抖动，手机端没必要） */
    public static short toRgb565(int argb) {
        int r = (argb >> 16) & 0xFF;
        int g = (argb >> 8) & 0xFF;
        int b = argb & 0xFF;
        return (short) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }

    public static byte[] encode(short[] px) {
        final int n = px.length;
        // 最坏情况：每个像素一个 raw token → 3 字节/像素
        byte[] out = new byte[n * 3 + 8];
        int o = 0;
        int i = 0;

        while (i < n) {
            int run = 1;
            while (i + run < n && run < 127 && px[i + run] == px[i]) {
                run++;
            }

            if (run >= 3) {
                out[o++] = (byte) (0x80 | run);
                out[o++] = (byte) (px[i] & 0xFF);
                out[o++] = (byte) ((px[i] >> 8) & 0xFF);
                i += run;
                continue;
            }

            // raw 段：一直收到下一个 >=4 的 run 为止
            int start = i;
            int cnt = 0;
            while (i < n && cnt < 127) {
                int r = 1;
                while (i + r < n && r < 4 && px[i + r] == px[i]) {
                    r++;
                }
                if (r >= 4) {
                    break;
                }
                i++;
                cnt++;
            }

            out[o++] = (byte) cnt;
            for (int k = start; k < start + cnt; k++) {
                out[o++] = (byte) (px[k] & 0xFF);
                out[o++] = (byte) ((px[k] >> 8) & 0xFF);
            }
        }

        byte[] exact = new byte[o];
        System.arraycopy(out, 0, exact, 0, o);
        return exact;
    }
}
