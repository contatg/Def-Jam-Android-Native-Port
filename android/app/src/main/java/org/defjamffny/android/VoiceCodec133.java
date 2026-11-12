package org.defjamffny.android;
/** IMA ADPCM wire adapter, from FFNY Online voice.c (MIT; see FFNY-ONLINE-LICENSE.txt).
 * 16 kHz mono, 640 samples / 40 ms; P_VOICE=7, 326-byte payload.
 */
final class VoiceCodec133 {
 static final int FRAME=640,PACKET=326;
 static final int[] STEP={
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767 },INDEX={ -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
 static final class State {int pred,index;}
 private static int clip(int x){return Math.max(-32768,Math.min(32767,x));}
 static int encodeNibble(State s,int sample){
  int step=STEP[s.index],diff=sample-s.pred,code=0,delta=step>>3;
  if(diff<0){code=8;diff=-diff;}
  if(diff>=step){code|=4;diff-=step;delta+=step;}
  if(diff>=(step>>1)){code|=2;diff-=step>>1;delta+=step>>1;}
  if(diff>=(step>>2)){code|=1;delta+=step>>2;}
  s.pred=clip(s.pred+((code&8)!=0?-delta:delta));s.index=Math.max(0,Math.min(88,s.index+INDEX[code]));return code;
 }
 static int decodeNibble(State s,int code){int step=STEP[s.index],delta=step>>3;
  if((code&4)!=0)delta+=step;if((code&2)!=0)delta+=step>>1;if((code&1)!=0)delta+=step>>2;
  s.pred=clip(s.pred+((code&8)!=0?-delta:delta));s.index=Math.max(0,Math.min(88,s.index+INDEX[code]));return s.pred;
 }
 static byte[] encode(short[] pcm,int seq,State s){byte[] b=new byte[PACKET];encodeInto(pcm,seq,s,b);return b;}
 static void encodeInto(short[] pcm,int seq,State s,byte[] b){if(pcm.length!=FRAME||b.length!=PACKET)throw new IllegalArgumentException("frame");
  b[0]=7;b[1]=(byte)seq;b[2]=(byte)(seq>>8);b[3]=(byte)s.pred;b[4]=(byte)(s.pred>>8);b[5]=(byte)s.index;
  for(int i=0;i<320;i++){int lo=encodeNibble(s,pcm[2*i]),hi=encodeNibble(s,pcm[2*i+1]);b[6+i]=(byte)(lo|hi<<4);}
 }
 static short[] decode(byte[] b){short[] pcm=new short[FRAME];decodeInto(b,0,pcm);return pcm;}
 static void decodeInto(byte[] b,int offset,short[] pcm){if(offset<0||b.length-offset!=PACKET||pcm.length!=FRAME||b[offset]!=7)throw new IllegalArgumentException("packet");
  State s=new State();s.pred=(short)((b[offset+3]&255)|((b[offset+4]&255)<<8));s.index=Math.min(88,b[offset+5]&255);
  for(int i=0;i<320;i++){pcm[2*i]=(short)decodeNibble(s,b[offset+6+i]&15);pcm[2*i+1]=(short)decodeNibble(s,(b[offset+6+i]&255)>>4);}
 }
 static boolean newer(int seq,int last){return (short)(seq-last)>0;}
}
