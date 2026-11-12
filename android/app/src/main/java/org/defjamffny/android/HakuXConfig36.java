// SPDX-License-Identifier: GPL-3.0-or-later
package org.defjamffny.android;

import java.util.ArrayList;
import java.util.List;

/** Arguments accepted by the included hakuX 1.3.0 core. */
final class HakuXConfig36 {
    final int scale;
    final String aspect, upscale;
    final boolean fxaa, async, prebuild, uncap, touch, fps;

    HakuXConfig36(int scale, String aspect, String upscale, boolean fxaa,
                  boolean async, boolean prebuild, boolean uncap, boolean touch, boolean fps) {
        this.scale = Math.max(1, Math.min(4, scale));
        this.aspect = isOneOf(aspect, "auto", "4:3", "16:9", "stretch") ? aspect : "auto";
        this.upscale = isOneOf(upscale, "bilinear", "sgsr") ? upscale : "bilinear";
        this.fxaa=fxaa; this.async=async; this.prebuild=prebuild;
        this.uncap=uncap; this.touch=touch; this.fps=fps;
    }
    private static boolean isOneOf(String value,String... options){
        for(String option:options)if(option.equals(value))return true;return false;
    }
    List<String> arguments(String disc,String hdd,int timezoneMinutes,String driver,boolean relaunch){
        ArrayList<String> args=new ArrayList<>();
        args.add(disc);args.add(hdd);args.add("padinput=sdl");args.add("pads=1");
        args.add("scale="+scale);args.add("aspect="+aspect);args.add("upscale="+upscale);
        args.add("fxaa="+(fxaa?"1":"0"));args.add("asyncpipe="+(async?"1":"0"));
        args.add("prebuild="+(prebuild?"1":"0"));args.add("tz="+timezoneMinutes);
        if(uncap)args.add("uncap");
        if(relaunch)args.add("relaunch");
        if(driver!=null&&!driver.isEmpty())args.add("vkdriver="+driver);
        return args;
    }
}
