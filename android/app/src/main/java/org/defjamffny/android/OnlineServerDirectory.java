package org.defjamffny.android;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.PBEKeySpec;

/** The public FFNY relay is built in. Connecting does not require a GitHub directory. */
final class OnlineServerDirectory {
    static String resolve(){return OnlineMenuState.DEFAULT_SERVER;}
    static String credential(String password)throws Exception{
        if(!OnlineMenuState.validPassword(password))throw new IOException("Use a password with 6 to 64 characters.");
        char[] chars=password.toCharArray();PBEKeySpec spec=new PBEKeySpec(chars,"contatg-defjam-private-room-v1".getBytes(StandardCharsets.US_ASCII),100000,256);
        try{
            byte[] key=SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256").generateSecret(spec).getEncoded();
            StringBuilder hex=new StringBuilder(64);for(byte b:key){hex.append(Character.forDigit((b>>>4)&15,16));hex.append(Character.forDigit(b&15,16));}Arrays.fill(key,(byte)0);return hex.toString();
        }finally{spec.clearPassword();Arrays.fill(chars,'\0');}
    }
    private OnlineServerDirectory(){}
}
