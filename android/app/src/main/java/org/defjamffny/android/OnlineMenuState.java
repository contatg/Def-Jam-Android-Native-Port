package org.defjamffny.android;

final class OnlineMenuState {
    static final String DEFAULT_SERVER="play.defjam.online";
    static final String NA_SERVER="na.defjam.online";
    static final String WEST_SERVER="west.defjam.online";
    static final int CONNECTION=0,LOBBY=1,ROOM=2,VOTE=3,STARTING=4,CREATE=5,JOIN=6;
    static boolean linked(int state){return state==4||state==6;}
    static int page(int state,boolean room,int phase){
        if(state==3&&room&&phase==2)return STARTING;
        if(!linked(state))return CONNECTION;
        if(!room)return LOBBY;
        return phase==1?VOTE:phase==2?STARTING:ROOM;
    }
    static boolean start(int state,boolean room,int phase,boolean requested,int desync){return requested&&state==4&&room&&phase==2&&desync==0;}
    static boolean validName(String s){return s!=null&&s.matches("[a-zA-Z0-9 _.\\-]{1,24}")&&!s.trim().isEmpty();}
    static boolean validCode(String s){return s!=null&&s.matches("[a-zA-Z0-9]{6}");}
    static boolean validPassword(String s){return s!=null&&s.length()>=6&&s.length()<=64;}
    static String endpoint(boolean custom,String address){return address!=null&&validServer(address.trim())&&!communityServer(address)?address.trim():"";}
    static String endpoint(boolean custom,String address,String region){return endpoint(true,address);}
    static boolean communityServer(String address){
        if(address==null||address.trim().isEmpty())return true;
        String host=address.trim().toLowerCase(java.util.Locale.ROOT);int colon=host.indexOf(':');if(colon>=0)host=host.substring(0,colon);
        if(host.endsWith("."))host=host.substring(0,host.length()-1);
        return host.equals("play.defjam.online")||host.equals("49.12.78.32")||host.equals("na.defjam.online")||host.equals("5.161.206.134")||host.equals("west.defjam.online")||host.equals("5.78.214.178");
    }
    static int roomMode(boolean hidden,boolean passwords){return !hidden?0:passwords?1:2;}
    static boolean validServer(String s){
        if(s==null||s.length()>120||!s.matches("[a-zA-Z0-9._-]+(?::[0-9]{1,5})?"))return false;
        int colon=s.lastIndexOf(':');if(colon<0)return true;
        try{int port=Integer.parseInt(s.substring(colon+1));return port>0&&port<=65535;}catch(NumberFormatException e){return false;}
    }
    private OnlineMenuState(){}
}
