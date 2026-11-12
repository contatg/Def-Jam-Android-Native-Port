package org.defjamffny.android;

import org.json.JSONArray;
import org.json.JSONObject;
import java.net.URI;
import java.util.Locale;

/** Release selection and version policy, independent of Android views/networking. */
final class ReleaseUpdate {
    static final String REPO = "contatg/Def-Jam-Android-Native-Port";
    static final String ROOT = "https://github.com/" + REPO;
    static final String RELEASES = ROOT + "/releases";
    static final String API = "https://api.github.com/repos/" + REPO + "/releases?per_page=30";
    final String name, tag, page, apk, metadata, notes, published, apkName, digest;
    final long apkSize;
    final boolean prerelease;
    long buildCode = -1;
    String version;

    private ReleaseUpdate(JSONObject r, JSONObject a, String meta) {
        tag=r.optString("tag_name"); name=r.optString("name",tag);
        page=r.optString("html_url"); apk=a.optString("browser_download_url"); metadata=meta;
        notes=r.optString("body"); published=r.optString("published_at");
        apkName=a.optString("name"); apkSize=a.optLong("size"); digest=a.optString("digest");
        prerelease=r.optBoolean("prerelease"); version=tag;
    }
    static boolean trusted(String url, String pathPrefix) {
        try {
            URI u=new URI(url);
            return "https".equals(u.getScheme()) && "github.com".equals(u.getHost())
                    && u.getRawUserInfo()==null && u.getPort()==-1 && u.getRawQuery()==null
                    && u.getRawFragment()==null && u.getPath().startsWith("/"+REPO+pathPrefix)
                    && !u.getPath().contains("/../") && !u.getPath().contains("/./");
        } catch(Exception e) { return false; }
    }
    static ReleaseUpdate fromFeed(String json) throws Exception {
        JSONArray feed=new JSONArray(json); ReleaseUpdate best=null;
        for(int i=0;i<feed.length();i++) {
            JSONObject r=feed.getJSONObject(i);
            if(r.optBoolean("draft") || r.isNull("published_at")
                    || r.optString("published_at").isEmpty()
                    || !trusted(r.optString("html_url"),"/releases/tag/")) continue;
            JSONArray assets=r.optJSONArray("assets"); if(assets==null)continue;
            JSONObject apk=null; String meta=""; int count=0;
            for(int j=0;j<assets.length();j++) {
                JSONObject a=assets.getJSONObject(j); String n=a.optString("name");
                String url=a.optString("browser_download_url");
                if(!"uploaded".equals(a.optString("state")) || !trusted(url,"/releases/download/"))continue;
                if(n.toLowerCase(Locale.ROOT).endsWith(".apk") && a.optLong("size")>0) {
                    apk=a;count++;
                }
                if("update.json".equals(n) && a.optLong("size")>0 && a.optLong("size")<=16384)meta=url;
            }
            // Never guess a device variant when a release contains several APKs.
            if(count!=1)continue;
            ReleaseUpdate candidate=new ReleaseUpdate(r,apk,meta);
            if(best==null || candidate.isPreferredTo(best))best=candidate;
        }
        return best;
    }
    private boolean isPreferredTo(ReleaseUpdate other) {
        boolean known = parts(tag) != null, otherKnown = parts(other.tag) != null;
        if(known != otherKnown)return known;
        int cmp=compareVersions(tag,other.tag);
        return cmp!=Integer.MIN_VALUE&&cmp!=0 ? cmp>0 : published.compareTo(other.published)>0;
    }
    void applyMetadata(String json) throws Exception {
        JSONObject m=new JSONObject(json);
        if(!"org.defjamffny.android".equals(m.optString("package_name")))throw new Exception("package");
        long code=m.optLong("version_code",-1);
        String hash=m.optString("apk_sha256").toLowerCase(Locale.ROOT);
        String v=m.optString("version_name");
        if(code<=0 || v.isEmpty() || !hash.matches("[a-f0-9]{64}")
                || !canonicalName(apkName).equals(canonicalName(m.optString("apk_name")))
                || apkSize!=m.optLong("apk_size",-1)
                || (!digest.isEmpty() && !("sha256:"+hash).equals(digest)))throw new Exception("asset");
        buildCode=code;
        if(parts(tag)==null)version=v;
    }
    private static String canonicalName(String n) { return n.replaceAll("[ ._-]+", "").toLowerCase(Locale.ROOT); }
    /** GitHub availability follows the public version. The APK build is displayed separately. */
    String status(String installedVersion) {
        int cmp=compareVersions(version,installedVersion);
        if(cmp==Integer.MIN_VALUE)return "Build information unavailable";
        if(cmp==0)return "You're up to date";
        return cmp>0?"Update available":"Your build is newer";
    }
    static int compareVersions(String a,String b) {
        String[] aa=parts(a),bb=parts(b);
        if(aa==null||bb==null)return Integer.MIN_VALUE;
        for(int i=0;i<Math.max(aa.length,bb.length);i++) {
            long x=i<aa.length?Long.parseLong(aa[i]):0,y=i<bb.length?Long.parseLong(bb[i]):0;
            if(x!=y)return Long.compare(x,y);
        }
        return 0;
    }
    private static String[] parts(String s) {
        if(s==null)return null;
        java.util.regex.Matcher m=java.util.regex.Pattern.compile("^[vV]?(\\d{1,8}(?:\\.\\d{1,8}){0,3})(?:[-+].*)?$").matcher(s);
        return m.matches()?m.group(1).split("\\."):null;
    }
}
