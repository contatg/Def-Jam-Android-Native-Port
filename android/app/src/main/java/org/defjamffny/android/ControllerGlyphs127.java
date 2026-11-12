package org.defjamffny.android;

/** Labels follow the actual Xbox input binding; the simulation mapping never changes. */
final class ControllerGlyphs127 {
    private static final String[] XBOX={"X","A","Y","B","LB","RB","LT","RT","START","BACK","MENU"};
    private static final String[] PS={"□","×","△","○","L1","R1","L2","R2","OPTIONS","SHARE","MENU"};
    static String label(int binding,boolean playStation){
        return binding>=0&&binding<XBOX.length?(playStation?PS:XBOX)[binding]:"";
    }
    static int color(int binding,boolean ps){
        if(ps){switch(binding){case 0:return 0xffe68aba;case 1:return 0xff7ea6ed;case 2:return 0xff68cbb2;case 3:return 0xffed818e;default:return 0xffe6ae43;}}
        switch(binding){case 0:return 0xff509cf6;case 1:return 0xff57d278;case 2:return 0xfff4c643;case 3:return 0xfff35a5e;default:return 0xffe6ae43;}
    }
}
