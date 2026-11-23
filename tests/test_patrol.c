#include "patrol.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static PatrolInput ready(void)
{
    PatrolInput i={0};
    i.fresh=i.healthy=i.nav_ok=i.prearm_ok=i.tof_ok=i.on_ground=true;
    i.height=0.15f; i.mode=5;
    return i;
}
static void qualify(Patrol *p, PatrolInput *i, uint32_t *now)
{
    for (unsigned n=0;n<60;n++) {
        *now+=200;
        assert(Patrol_Update(p,i,*now,0).type==PATROL_NONE);
    }
}
static void begin(Patrol *p, PatrolInput *i, uint32_t *now)
{
    Patrol_Init(p); *i=ready(); qualify(p,i,now);
    *now+=200;
    assert(Patrol_Update(p,i,*now,1).type==PATROL_SET_MODE);
    i->mode=4; *now+=200;
    assert(Patrol_Update(p,i,*now,0).type==PATROL_NONE);
    *now+=200;
    assert(Patrol_Update(p,i,*now,0).type==PATROL_ARM_CMD);
    i->armed=true; *now+=200;
    assert(Patrol_Update(p,i,*now,0).type==PATROL_NONE);
    *now+=200;
    PatrolAction a=Patrol_Update(p,i,*now,0);
    assert(a.type==PATROL_TAKEOFF_CMD && fabsf(a.z-2.85f)<0.001f);
}
int main(void)
{
    Patrol p; PatrolInput i=ready(); uint32_t now=0;
    Patrol_Init(&p);
    assert(Patrol_Update(&p,&i,200,1).type==PATROL_NONE); /* No instant arm. */
    qualify(&p,&i,&now);
    i.prearm_ok=false;
    assert(Patrol_Update(&p,&i,now+200,1).type==PATROL_NONE && p.state==PATROL_IDLE);
    begin(&p,&i,&now);
    /* Simple kinematic vehicle follows transmitted targets, including all
     * turns and ToF-based z corrections. It must return to launch XY. */
    i.on_ground=false; i.z=-3; i.height=3;
    unsigned corners=0; PatrolState previous=p.state;
    for (unsigned n=0;n<1500 && p.state!=PATROL_HOVER;n++) {
        now+=200;
        PatrolAction a=Patrol_Update(&p,&i,now,0);
        assert(a.type!=PATROL_ARM_CMD && a.type!=PATROL_TAKEOFF_CMD);
        assert(p.state!=PATROL_LANDING && p.state!=PATROL_IDLE);
        if (p.state==PATROL_TURN && previous!=PATROL_TURN) corners++;
        if (a.type==PATROL_TARGET) {
            assert(hypotf(a.x-i.x,a.y-i.y)<0.61f);
            i.x=a.x; i.y=a.y;
            i.height-=a.z-i.z; i.z=a.z; i.yaw=a.yaw;
        }
        previous=p.state;
    }
    assert(p.state==PATROL_HOVER && corners==4 && p.leg==4);
    assert(hypotf(i.x,i.y)<0.8f && fabsf(i.height-3)<0.05f);
    /* Height is corrected toward 3m from the range reference, not GPS MSL. */
    i.height=3.4f; now+=200;
    PatrolAction a=Patrol_Update(&p,&i,now,0);
    assert(a.type==PATROL_TARGET && a.z>i.z && a.z-i.z<=0.251f);
    /* A mode change hands control back without commanding Guided again. */
    i.mode=2; now+=200;
    assert(Patrol_Update(&p,&i,now,0).type==PATROL_NONE && p.state==PATROL_IDLE);
    begin(&p,&i,&now); i.tof_ok=false; now+=200;
    a=Patrol_Update(&p,&i,now,0);
    assert(a.type==PATROL_SET_MODE && a.mode==9);
    begin(&p,&i,&now); i.nav_ok=false; now+=200;
    a=Patrol_Update(&p,&i,now,0);
    assert(a.type==PATROL_SET_MODE && a.mode==9);
    begin(&p,&i,&now); i.fresh=false; now+=200;
    assert(Patrol_Update(&p,&i,now,0).type==PATROL_NONE && p.state==PATROL_LANDING);
    i.fresh=true; now+=200;
    a=Patrol_Update(&p,&i,now,0); assert(a.type==PATROL_SET_MODE && a.mode==9);
    begin(&p,&i,&now); now+=200;
    a=Patrol_Update(&p,&i,now,2); assert(a.type==PATROL_TARGET && p.state==PATROL_HOVER);
    now+=200;
    a=Patrol_Update(&p,&i,now,3); assert(a.type==PATROL_SET_MODE && a.mode==9);
    /* A negative takeoff ACK never advances to the rectangle. */
    begin(&p,&i,&now); i.ack_serial++; i.ack_command=22; i.ack_result=2; now+=200;
    a=Patrol_Update(&p,&i,now,0); assert(a.type==PATROL_SET_MODE && a.mode==9);
    begin(&p,&i,&now); now+=31000;
    a=Patrol_Update(&p,&i,now,0); assert(a.type==PATROL_SET_MODE && a.mode==9);
    /* millis wrap must not bypass qualification or break leg deadlines. */
    now=0xffffe000U; begin(&p,&i,&now); assert(p.state==PATROL_TAKEOFF);
    puts("Patrol: rectangle/ToF/hover, readiness, takeover, stop/land, link loss, rejection, timeout and tick-wrap passed");
    return 0;
}
