"""Exercise the production patrol state machine against an isolated ArduCopter SITL.

Linux: python3 tests/patrol_sitl.py build/patrol-sitl
Requires gcc, pymavlink, and arducopter + copter.parm in the given directory.
Only connects to a simulator started by this process on localhost TCP 5785.
The rangefinder input is generated from simulator truth, not a real TFmini.
"""
import ctypes as C
import json
import math
from pathlib import Path
import subprocess
import sys
import time
from pymavlink import mavutil

ROOT = Path(__file__).resolve().parents[1]
WORK = Path(sys.argv[1]).resolve()

class Input(C.Structure):
    _fields_ = [(n, C.c_bool) for n in ('fresh','healthy','nav_ok','prearm_ok','tof_ok','armed','on_ground')] + [
        ('mode',C.c_uint32)] + [(n,C.c_float) for n in ('x','y','z','vx','vy','vz','yaw','height')] + [
        ('ack_serial',C.c_uint32),('ack_command',C.c_uint16),('ack_result',C.c_uint8)]

class Action(C.Structure):
    _fields_ = [('type',C.c_int),('mode',C.c_uint32)] + [(n,C.c_float) for n in ('x','y','z','yaw')]

def main():
    shim = WORK/'shim.c'
    shim.write_text('''#include "patrol.h"
static Patrol p;
void reset(void) {Patrol_Init(&p);}
void step(const PatrolInput *i,unsigned now,unsigned command,PatrolAction *a) {*a=Patrol_Update(&p,i,now,command);}
int state(void) {return p.state;}
unsigned leg(void) {return p.leg;}
const char *reason(void) {return p.reason;}
unsigned input_size(void) {return sizeof(PatrolInput);}
''')
    subprocess.run(['gcc','-shared','-fPIC','-I'+str(ROOT/'Core/Inc'),str(shim),
                    str(ROOT/'Core/Src/patrol.c'),'-lm','-o',str(WORK/'libpatrol.so')],check=True)
    lib=C.CDLL(str(WORK/'libpatrol.so'))
    lib.step.argtypes=[C.POINTER(Input),C.c_uint32,C.c_uint,C.POINTER(Action)]
    lib.reason.restype=C.c_char_p
    assert lib.input_size()==C.sizeof(Input)
    lib.reset()
    defaults=WORK/'patrol.parm'
    defaults.write_text((WORK/'copter.parm').read_text()+'''
RNGFND1_TYPE 10
RNGFND1_MIN 0.1
RNGFND1_MAX 10
RNGFND1_ORIENT 25
RNGFND1_GNDCLR 0.1
SIM_GPS_NUMSATS 15
''')
    log=(WORK/'sitl-console.log').open('w')
    process=subprocess.Popen([str(WORK/'arducopter'),'-w','--model','quad','--speedup','3',
        '--home','-35.363261,149.165230,584,0','--defaults',str(defaults),
        '--serial0','tcp:5785:wait'],cwd=WORK,stdout=log,stderr=subprocess.STDOUT)
    connection=None
    events=[]
    try:
        for _ in range(50):
            try:
                connection=mavutil.mavlink_connection('tcp:127.0.0.1:5785',source_system=255,source_component=191)
                break
            except OSError:
                if process.poll() is not None: raise RuntimeError('SITL exited; inspect sitl-console.log')
                time.sleep(0.1)
        assert connection is not None
        m=connection
        m.wait_heartbeat(timeout=15)
        ids=[1,30,32,132,193,245,108]
        for msgid in ids:
            m.mav.command_long_send(1,1,511,0,msgid,100000 if msgid in (30,32,132,108) else 500000,0,0,0,0,0)
        latest={}; seen={}; now=0; next_tick=0; last_inject=-1000; last_rc=-1000
        inp=Input(); out=Action(); started=False; old=None; hover_start=None
        started_wall=time.monotonic(); last_wall_report=started_wall
        while time.monotonic()-started_wall < 200:
            msg=m.recv_match(blocking=True,timeout=0.2)
            if msg is None: continue
            kind=msg.get_type()
            if hasattr(msg,'time_boot_ms'): now=max(now,msg.time_boot_ms)
            latest[kind]=msg; seen[kind]=now
            if kind=='STATUSTEXT':
                print('FC:',msg.text,flush=True)
            if kind=='COMMAND_ACK' and msg.command in (22,400):
                inp.ack_command=msg.command; inp.ack_result=msg.result; inp.ack_serial+=1
                print('ACK',msg.command,msg.result,flush=True)
            if now-last_rc>=200:
                m.mav.rc_channels_override_send(1,1,1500,1500,1000,1500,1800,1500,1500,1500)
                last_rc=now
            truth=latest.get('SIM_STATE')
            attitude=latest.get('ATTITUDE')
            if truth and attitude and now-last_inject>=100:
                # Simulator's home MSL=584. Offset=ground sensor clearance.
                height=max(0.1,truth.alt-584.0+0.1)
                slant=height/max(0.8,math.cos(attitude.roll)*math.cos(attitude.pitch))
                m.mav.distance_sensor_send(now,10,1000,min(999,int(slant*100)),0,0,25,0)
                last_inject=now
            if now<next_tick: continue
            next_tick=now+200
            hb=latest.get('HEARTBEAT'); status=latest.get('SYS_STATUS'); pos=latest.get('LOCAL_POSITION_NED')
            ekf=latest.get('EKF_STATUS_REPORT'); ground=latest.get('EXTENDED_SYS_STATE'); rng=latest.get('DISTANCE_SENSOR')
            required={'HEARTBEAT':2000,'SYS_STATUS':2500,'LOCAL_POSITION_NED':800,'ATTITUDE':800,'EXTENDED_SYS_STATE':1500}
            inp.fresh=all(k in seen and now-seen[k]<age for k,age in required.items())
            if hb:
                inp.armed=bool(hb.base_mode&128); inp.mode=hb.custom_mode
            if status:
                enabled=status.onboard_control_sensors_enabled&status.onboard_control_sensors_present&~(1<<28)
                inp.healthy=enabled!=0 and enabled&~status.onboard_control_sensors_health==0 and hb.system_status in (3,4)
                inp.prearm_ok=bool(status.onboard_control_sensors_present&(1<<28) and status.onboard_control_sensors_enabled&(1<<28) and status.onboard_control_sensors_health&(1<<28))
            if pos:
                for n in ('x','y','z','vx','vy','vz'): setattr(inp,n,getattr(pos,n))
            if attitude: inp.yaw=attitude.yaw
            inp.nav_ok=bool(ekf and now-seen['EKF_STATUS_REPORT']<1500 and ekf.flags&0x37==0x37 and not ekf.flags&(128|1024|32768))
            inp.on_ground=bool(ground and ground.landed_state==1)
            inp.tof_ok=bool(rng and now-seen['DISTANCE_SENSOR']<500 and rng.orientation==25 and rng.type==0 and rng.min_distance<=rng.current_distance<rng.max_distance and rng.current_distance>0 and rng.max_distance>=400 and getattr(rng,'signal_quality',0)!=1 and attitude and abs(attitude.roll)<0.5 and abs(attitude.pitch)<0.5)
            if rng and attitude: inp.height=rng.current_distance*0.01*math.cos(attitude.roll)*math.cos(attitude.pitch)
            command=0
            if not started and now>20000 and inp.fresh and inp.healthy and inp.nav_ok and inp.prearm_ok and inp.tof_ok:
                command=1 # Rejected requests never queue delayed automatic starts.
            lib.step(C.byref(inp),now,command,C.byref(out))
            state=lib.state()
            if state!=0: started=True
            label=(state,lib.leg(),lib.reason().decode())
            if label!=old:
                row={'ms':now,'state':label[0],'leg':label[1],'reason':label[2],'x':inp.x,'y':inp.y,'height':inp.height}
                events.append(row); print(row,flush=True); old=label
            if time.monotonic()-last_wall_report>10:
                print('CHECK',now,inp.fresh,inp.healthy,inp.nav_ok,inp.prearm_ok,inp.tof_ok,inp.height,flush=True)
                last_wall_report=time.monotonic()
            if out.type==1:
                m.mav.command_long_send(1,1,176,0,1,out.mode,0,0,0,0,0)
            elif out.type==2:
                m.mav.command_long_send(1,1,400,0,1,0,0,0,0,0,0)
            elif out.type==3:
                m.mav.command_long_send(1,1,22,0,0,0,0,0,0,0,out.z)
            elif out.type==4:
                m.mav.set_position_target_local_ned_send(now,1,1,1,0x09f8,out.x,out.y,out.z,0,0,0,0,0,0,out.yaw,0)
            if started and state in (0,7): raise AssertionError('Patrol aborted: '+lib.reason().decode())
            if state==6:
                if hover_start is None: hover_start=now
                if now-hover_start>5000:
                    assert lib.leg()==4
                    assert abs(inp.height-3)<0.35
                    assert math.hypot(inp.x-events[2]['x'],inp.y-events[2]['y'])<2.0
                    print('SITL rectangle and five-second ToF-referenced hover PASSED',flush=True)
                    (WORK/'result.json').write_text(json.dumps({'passed':True,'events':events},indent=2))
                    return
        raise TimeoutError('SITL patrol did not complete')
    finally:
        if connection: connection.close()
        process.terminate()
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
        log.close()

if __name__=='__main__': main()
