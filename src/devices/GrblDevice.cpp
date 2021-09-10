#include "GrblDevice.h"
#include "printfloat.h"


    void GrblDevice::sendProbe(Stream &serial) {
        serial.print("\n$I\n");
    }

    bool GrblDevice::checkProbeResponse(const String v) {
        if(v.indexOf("[VER:")!=-1 ) {
            GD_DEBUGS("Detected GRBL device");
            return true;
        }
        return false;
    }


    bool GrblDevice::jog(uint8_t axis, float dist, int feed) {
        constexpr static char AXIS[] = {'X', 'Y', 'Z'};
        constexpr size_t LN=25;
        char msg[LN]; 
        int l = snprintf(msg, LN, "$J=G91 F%d %c", feed, AXIS[axis]);
        snprintfloat(msg+l, LN-l, dist, 3 );
        return scheduleCommand(msg, strlen(msg) );
    }
        
    bool GrblDevice::canJog() {        
        return status=="Idle" || status=="Jog";
        
    }

    bool GrblDevice::isCmdRealtime(const char* data, size_t len) {
        if (len != 1) return false;
        char c = data[0];
        switch(c) {
            case '?': // status
            case '~': // cycle start/stop
            case '!': // feedhold
            case 0x18: // ^x, reset
            case 0x84: // door
            case 0x85: // jog cancel  
            case 0x9E: // toggle spindle
            case 0xA0: // toggle flood coolant
            case 0xA1: // toggle mist coolant      
            case 0x90 ... 0x9D: // feed override, rapid override, spindle override
                return true;
            default:
                return false;
        }
    }

    void GrblDevice::trySendCommand() {

        // if(isCmdRealtime(curUnsentPriorityCmd, curUnsentPriorityCmdLen) ) {
        //     printerSerial->write(curUnsentPriorityCmd, curUnsentPriorityCmdLen);  
        //     GD_DEBUGF("<  (f%3d,%3d) '%c' RT\n", sentCounter->getFreeLines(), sentCounter->getFreeBytes(), curUnsentPriorityCmd[0] );
        //     curUnsentPriorityCmdLen = 0;
        //     return;
        // }

        char* cmd  = curUnsentPriorityCmdLen!=0 ? &curUnsentPriorityCmd[0] :  &curUnsentCmd[0]; 
        size_t &len = curUnsentPriorityCmdLen!=0 ? curUnsentPriorityCmdLen : curUnsentCmdLen ;

        cmd[len]=0;
        if( sentCounter->canPush(len) ) {
            sentCounter->push( cmd, len );
            printerSerial->write( (const uint8_t*)cmd, len);  
            printerSerial->write('\n');
            GD_DEBUGF("<  (f%3d,%3d) '%s'(len %d)\n", sentCounter->getFreeLines(), sentCounter->getFreeBytes(), cmd, len );
            len = 0;
        } else {
            GD_DEBUGF("<  (f%3d,%3d) NO SPACE: '%s'(len %d)\n", sentQueue.getFreeLines() , sentQueue.getFreeBytes(), cmd, len  );
        }

    }

    void GrblDevice::tryParseResponse( char* resp, size_t len ) {
        if (startsWith(resp, "ok")) {
            sentQueue.pop();
            connected = true;
            panic = false;
            lastResponse = "";
            notify_observers(DeviceStatusEvent{0}); 
        } else 
        if ( startsWith(resp, "<") ) {
            parseGrblStatus(resp+1);
            panic = false;
        } else 
        if (startsWith(resp, "error") ) {
            sentQueue.pop();
            GD_DEBUGF("ERR '%s'\n", resp ); 
            notify_observers(DeviceStatusEvent{1}); 
            lastResponse = resp;
        } else
        if ( startsWith(resp, "ALARM:") ) {
            panic = true;
            GD_DEBUGF("ALARM '%s'\n", resp ); 
            lastResponse = resp;
            // no mor status updates will come in, so update status.
            status = "Alarm";
            notify_observers(DeviceStatusEvent{2}); 
        } else 
        if(startsWith(resp, "[MSG:")) {
            GD_DEBUGF("Msg '%s'\n", resp ); 
            resp[len-1]=0; // strip last ']'
            lastResponse = resp+5;
            // this is the first message after reset
            notify_observers(DeviceStatusEvent{3}); 
        }        
        
        GD_DEBUGF(" > (f%3d,%3d) '%s'(len %d) \n", sentQueue.getFreeLines(), sentQueue.getFreeBytes(),resp, len );
    }

    void mystrcpy(char* dst, const char* start, const char* end) {
        while(start!=end) {
            *(dst++) = *(start++);
        }
        *dst=0;
    }

    void GrblDevice::parseGrblStatus(char* v) {
        //<Idle|MPos:9.800,0.000,0.000|FS:0,0|WCO:0.000,0.000,0.000>
        //<Idle|MPos:9.800,0.000,0.000|FS:0,0|Ov:100,100,100>
        //GD_DEBUGF("parsing %s\n", v.c_str() );
        
        char buf[10];
        bool mpos;
        char cpy[100];
        strncpy(cpy, v, 100);
        v=cpy;

        // idle/jogging
        char* pch = strtok(v, "|");
        if(pch==nullptr) return;
        status = pch; 
        if( strcmp(pch, "Alarm")==0 ) panic=true;
        //GD_DEBUGF("Parsed Status: %s\n", status.c_str() );

        // MPos:0.000,0.000,0.000
        pch = strtok(nullptr, "|"); 
        if(pch==nullptr) return;
        
        char *st, *fi;
        st=pch+5;fi = strchr(st, ',');   mystrcpy(buf, st, fi);  x = _atod(buf);
        st=fi+1; fi = strchr(st, ',');   mystrcpy(buf, st, fi);  y = _atod(buf);
        st=fi+1;                                                 z = _atod(st);
        mpos = startsWith(pch, "MPos");
        //GD_DEBUGF("Parsed Pos: %f %f %f\n", x,y,z);

        // FS:500,8000 or F:500    
        pch = strtok(nullptr, "|"); 
        while(pch!=nullptr) {
        
            if( startsWith(pch, "FS:") || startsWith(pch, "F:")) {
                if(pch[1] == 'S') {
                    st=pch+3; fi = strchr(st, ','); mystrcpy(buf, st, fi);  feed = atoi(buf);
                    st=fi+1;  spindleVal = atoi(st);
                } else {
                    feed = atoi(pch+2);
                }
            } else 
            if(startsWith(pch, "WCO:")) {
                st=pch+4;fi = strchr(st, ',');   mystrcpy(buf, st, fi);  ofsX = _atod(buf);
                st=fi+1; fi = strchr(st, ',');   mystrcpy(buf, st, fi);  ofsY = _atod(buf);
                st=fi+1;                                                 ofsZ = _atod(st);
                //GD_DEBUGF("Parsed WCO: %f %f %f\n", ofsX, ofsY, ofsZ);
            }

            pch = strtok(nullptr, "|"); 

        }
        
        if(!mpos) {
            x -= ofsX; y -= ofsY; z -= ofsZ;
        }
        
        notify_observers(DeviceStatusEvent{0});
    }

