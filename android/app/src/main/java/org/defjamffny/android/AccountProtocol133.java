package org.defjamffny.android;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
/** The PC service hashes the 64 ASCII hex characters, not the original 32 random bytes. */
final class AccountProtocol133 {
 static String userAgent(long build) { return "FFNY-Online-Android/" + build; }
 static byte[] hashInstallId(byte[] seed) throws Exception {
  if (seed.length != 32) throw new IllegalArgumentException("Install ID seed length");
  return java.util.Arrays.copyOf(MessageDigest.getInstance("SHA-256").digest(seed), 16);
 }
 static String hex(byte[] b){StringBuilder s=new StringBuilder(b.length*2);for(byte x:b){int n=x&255;s.append("0123456789abcdef".charAt(n>>>4)).append("0123456789abcdef".charAt(n&15));}return s.toString();}
 static String loginHash(String secret)throws Exception{
  if(secret==null||!secret.matches("[0-9a-f]{64}"))throw new IllegalArgumentException("login secret");
  return hex(MessageDigest.getInstance("SHA-256").digest(secret.getBytes(StandardCharsets.US_ASCII)));
 }
}
