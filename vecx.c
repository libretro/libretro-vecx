// Malban: September 2022
// corrected 6809 emulation
// corrected bankswitching up to VB
// fixed SHIFT register
// added support for "bad monitors"
// done: flash
// done: drift
// done: autosync
// done: bad apple: auto loads "filename.vvm" as movie file! File completely loaded into one large buffer!
//       save for this does not work works, filename of move is derived from libretro.c variable
//		 file position is not saved, and no fseek(...) implemented - don't save a movie!
// done: samples (Spike, Movies)
// done: correct save states (with all my added vars)
//
// not done: (won't do?)
// better GFX -> shader related
// Speakjet -> perhaps in the future
// Lightpen -> NO
// 3d Imager -> NO
// diverse OneWireChips -> NO
// extra RAM -> NO

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "e6809.h"
#include "vecx.h"
#include "osint.h"
#include "e8910.h"

#define einline __inline

char *getMoviePath(); 				// libretro.c, path of the cartridge, we derive the movie name from this
extern unsigned char *movieBuffer;	// the movie will be loaded here to, the memory is "malloced", and freed by libretro upon leave

// this could be optimized with a linked list!
// I never encountered more than 10
// if more timers are needed, core will EXIT!!!
// this is not dynamic
#define MAX_TIMER 10
typedef struct TimerItem_s 
{
	signed int countDown;
	unsigned char valueToSet;
	int *whereToSet;
	int type;
	int active;
} TimerItem;
TimerItem timerItemArray[MAX_TIMER]; // should be saved

// these are types, and Array Index, both!
enum
{
    TIMER_ACTION_NONE = 0,
    TIMER_ZERO = 1,
    TIMER_BLANK_ON_CHANGE = 2,
    TIMER_BLANK_OFF_CHANGE = 3,
    TIMER_RAMP_CHANGE = 4,
    TIMER_MUX_Y_CHANGE = 5,
    TIMER_MUX_S_CHANGE = 6,
    TIMER_MUX_Z_CHANGE = 7,
    TIMER_MUX_R_CHANGE = 8,
    TIMER_XSH_CHANGE = 9,
    TIMER_LIGHTPEN = 10,
    TIMER_RAMP_OFF_CHANGE = 11,
    TIMER_MUX_SEL_CHANGE = 12,
    TIMER_SHIFT = 13,
    TIMER_T1 = 14,
    TIMER_T2 = 15,
	TIMER_SHIFT_WRITE = 13+1024, // the delay is the normal "SHIFT"
    TIMER_SHIFT_READ = 13+2048, // the delay is the normal "SHIFT"
};

int DELAYS[]={
	0,  // TIMER_ACTION_NONE = 0,
	5,  // TIMER_ZERO = 1,
	0,  // TIMER_BLANK_ON_CHANGE = 2,
	0,  // TIMER_BLANK_OFF_CHANGE = 3,
	12, // TIMER_RAMP_CHANGE = 4,
	14, // TIMER_MUX_Y_CHANGE = 5,
	0,  // TIMER_MUX_S_CHANGE = 6,
	0,  // TIMER_MUX_Z_CHANGE = 7,
	0,  // TIMER_MUX_R_CHANGE = 8,
	15, // TIMER_XSH_CHANGE = 9,
	0,  // TIMER_LIGHTPEN = 10,
	15, // TIMER_RAMP_OFF_CHANGE = 11,
	1,  // TIMER_MUX_SEL_CHANGE = 12, 
	0,  // TIMER_SHIFT = 13, 
	0,  // TIMER_T1 = 14, 
	0   // TIMER_T2 = 15,
	}; // no need to be saved

// no need to be saved
// set from libretro "core config"
static double config_drift_x = 0.09;
static double config_drift_y = -.04;
int config_autoSync = 1;

// accessable from retrolib.c
void setDrift(double x, double y)
{
	config_drift_x = x;
	config_drift_y = y;
}

// only rudimentary
// don't think it needs to be saved
#define FLASH_SUPPORT 1
#ifdef FLASH_SUPPORT
int flashSupport = 0;
extern int addressBUS;
extern unsigned char dataBUS;
int idSequenceAddress = 0;
int idSequenceData = 0;

int eraseAddress = 0;
int eraseSequenceAddress = 0;
int eraseSequenceData = 0;

int writeSequenceAddress = 0;
int writeSequenceData = 0;
#endif
int flashcartChanged=0;

// carts support vectorblade now -> 256K	
#define MAX_CART_SIZE (0x10000*4)
#define VECTREX_MHZ 1500000
#define JOYSTICK_CENTER 0x7f // for whatever reason Debris does not like $80, $7f is ok though - is that a emulator test?

enum
{
   VECTREX_PDECAY	 = 30,      /* phosphor decay rate */

   /* number of 6809 cycles before a frame redraw */

   FCYCLES_INIT    = VECTREX_MHZ / VECTREX_PDECAY,

   /* max number of possible vectors that maybe on the screen at one time.
    * one only needs VECTREX_MHZ / VECTREX_PDECAY but we need to also store
    * deleted vectors in a single table
    */

   VECTOR_CNT		 = VECTREX_MHZ / VECTREX_PDECAY,

   VECTOR_HASH     = 65521
};


unsigned char rom[8192];
unsigned char cart[MAX_CART_SIZE];
unsigned char * getCart(){return cart;} // used by libretro to save flash!

unsigned char vecx_ram[1024];

/* the via 6522 registers */
static unsigned via_ora;
static unsigned via_orb;
static unsigned via_ddra;
static unsigned via_ddrb;
static unsigned via_t1on;  /* is timer 1 on? */
static unsigned via_t1int; /* are timer 1 interrupts allowed? */
static unsigned via_t1c;
static unsigned via_t1ll;
static unsigned via_t1lh;
static unsigned via_t1pb7; /* timer 1 controlled version of pb7 */
static unsigned via_t2on;  /* is timer 2 on? */
static unsigned via_t2int; /* are timer 2 interrupts allowed? */
static unsigned via_t2c;
static unsigned via_t2ll;
static unsigned via_sr;
static unsigned via_srb;   /* number of bits shifted so far */
static unsigned via_src;   /* shift counter */
static unsigned via_srclk;
static unsigned via_acr;
static unsigned via_pcr;
static unsigned via_ifr;
static unsigned via_ier;
static unsigned old_via_ca1;
static unsigned via_ca1;
static unsigned via_ca2;
static unsigned via_cb2h;  /* basic handshake version of cb2 */
static unsigned via_cb2s;  /* version of cb2 controlled by the shift register */

/* analog devices */

static unsigned alg_sel;
static unsigned alg_ramping;
static signed alg_DAC;  /* z sample and hold */
static signed alg_ssh;
static signed alg_rsh;  /* zero ref sample and hold */
static signed alg_xsh;  /* x sample and hold */
static signed alg_ysh;  /* y sample and hold */
static signed alg_zsh;  /* z sample and hold */
static signed alg_jsh;  /* joystick sample and hold */
unsigned alg_jch0;		  /* joystick direction channel 0 */
unsigned alg_jch1;		  /* joystick direction channel 1 */
unsigned alg_jch2;		  /* joystick direction channel 2 */
unsigned alg_jch3;		  /* joystick direction channel 3 */

static unsigned alg_compare;

static double alg_curr_x; /* current x position */
static double alg_curr_y; /* current y position */

static unsigned alg_vectoring; /* are we drawing a vector right now? */
static long alg_vector_x0;
static long alg_vector_y0;
static long alg_vector_x1;
static long alg_vector_y1;
static long alg_vector_dx;
static long alg_vector_dy;
static int alg_vector_speed;
static unsigned char alg_vector_color;

long vector_draw_cnt;
long vector_erse_cnt;
static vector_t vectors_set[2 * VECTOR_CNT];
vector_t *vectors_draw;
vector_t *vectors_erse;

static long vector_hash[VECTOR_HASH];

static long fcycles;

static unsigned snd_select;
unsigned snd_regs[16];

// new
unsigned long int ticksRunning = 0;
int intensityDrift = 0;
long lastAddLine = 0;
long cyclesRunning = 0;
int thisWaitRecal = 0;
long lastWaitRecal=0;
long lastSyncCycles = 0;
int syncImpulse = 0;

static int cartlen = 0;
static int sig_ramp = 0; 
static int sig_blank = 0; // moved out of emu_loop for lightpen access
static int sig_zero = 0;
static int alternate = 0;
static int currentBank = 0;
static int currentIRQ = 1;
static int currentPB6 = 1;
static int pb6_in = 0x40; // 0 or 0x40 in from external
static int pb6_out = 0x40; // out from vectrex
static int BANK_MAX = 1;

static int resistorOhm = 175;
static double supplyVoltage=0;

static double capacitorFarad = 0.000000006;
static double currentVoltage=0;
static double timeConstant = 0;

static double VECTREX_CYCLE_TIME = (double)(1.0/1500000.0);
static double percentageDifChangePerCycle = 0;
static int stepsDone = 0;

void timerAddItem(int value, void *destination, int t);



unsigned char get_cart(unsigned pos)
{
   if (BANK_MAX<4) return cart[pos+(currentBank *32768)] & 0xff; // 
   return cart[pos+(currentBank *65536)] & 0xff; // 
}

void set_cart(unsigned pos, unsigned char data)
{
   cart[(pos)%MAX_CART_SIZE] = data; 
}

void set_cartSize(int size)
{
	cartlen = size;
}

// this might be done "nicer" - 
// didn'T think about it much.... it works... so be it...
static einline signed int makeSigned(unsigned char data)
{
   if (data > 127) return -(256-data);
   return data;
}

static einline unsigned char makeUnsigned(signed int data)
{
   return data&0xff;
}

int vecx_statesz(void)
{
   return 1025 + (sizeof(unsigned) * (60)) + (sizeof(long) * 14)+ (sizeof(double) * 8) +(sizeof(TimerItem) *MAX_TIMER)
          + e6809_statesz() + e8910_statesz();
}
int vecx_serialize(char* dst, int size)
{
   if (size < vecx_statesz())
      return 0;

   e6809_serialize(dst);
   dst += e6809_statesz();
   e8910_serialize(dst);
   dst += e8910_statesz();

   memcpy(dst, vecx_ram, 1024);
   dst += 1024;

   memcpy(dst, &snd_select, sizeof(int)); dst += sizeof(int); 
   memcpy(dst, &via_ora,    sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_orb,    sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_ddra,   sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_ddrb,   sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_t1on,   sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_t1int,  sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_t1c,    sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_t1ll,   sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_t1lh,   sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_t1pb7,  sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_t2on,   sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_t2int,  sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_t2c,    sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_t2ll,   sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_sr,     sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_srb,    sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_src,    sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_srclk,  sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_acr,    sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_pcr,    sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_ifr,    sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_ier,    sizeof(int)); dst += sizeof(int);
   memcpy(dst, &old_via_ca1,sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_ca1,    sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_ca2,    sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_cb2h,   sizeof(int)); dst += sizeof(int);
   memcpy(dst, &via_cb2s,   sizeof(int)); dst += sizeof(int);
   memcpy(dst, &alg_rsh,    sizeof(int)); dst += sizeof(int);
   memcpy(dst, &alg_xsh,    sizeof(int)); dst += sizeof(int);
   memcpy(dst, &alg_ysh,    sizeof(int)); dst += sizeof(int);
   memcpy(dst, &alg_zsh,    sizeof(int)); dst += sizeof(int);
   memcpy(dst, &alg_jch0,   sizeof(int)); dst += sizeof(int);
   memcpy(dst, &alg_jch1,   sizeof(int)); dst += sizeof(int);
   memcpy(dst, &alg_jch2,   sizeof(int)); dst += sizeof(int);
   memcpy(dst, &alg_jch3,   sizeof(int)); dst += sizeof(int);
   memcpy(dst, &alg_jsh,    sizeof(int)); dst += sizeof(int);
   memcpy(dst, &alg_compare, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &alg_vectoring, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &alg_sel, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &alg_ramping, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &alg_DAC, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &alg_ssh, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &alg_vector_speed, sizeof(int)); dst += sizeof(int);

   memcpy(dst, &stepsDone, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &sig_blank, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &alternate, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &currentBank, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &currentIRQ, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &currentPB6, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &BANK_MAX, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &resistorOhm, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &intensityDrift, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &thisWaitRecal, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &syncImpulse, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &sig_ramp, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &sig_zero, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &pb6_in, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &pb6_out, sizeof(int)); dst += sizeof(int);
   memcpy(dst, &cartlen, sizeof(int)); dst += sizeof(int);

   memcpy(dst, &alg_vector_x0, sizeof(long)); dst += sizeof(long);
   memcpy(dst, &alg_vector_y0, sizeof(long)); dst += sizeof(long);
   memcpy(dst, &alg_vector_x1, sizeof(long)); dst += sizeof(long);
   memcpy(dst, &alg_vector_y1, sizeof(long)); dst += sizeof(long);
   memcpy(dst, &alg_vector_dx, sizeof(long)); dst += sizeof(long);
   memcpy(dst, &alg_vector_dy, sizeof(long)); dst += sizeof(long);
   memcpy(dst, &vector_draw_cnt, sizeof(long)); dst += sizeof(long);
   memcpy(dst, &vector_erse_cnt, sizeof(long)); dst += sizeof(long);
   memcpy(dst, &lastAddLine, sizeof(long)); dst += sizeof(long);
   memcpy(dst, &cyclesRunning, sizeof(long)); dst += sizeof(long);
   memcpy(dst, &lastWaitRecal, sizeof(long)); dst += sizeof(long);
   memcpy(dst, &lastSyncCycles, sizeof(long)); dst += sizeof(long);
   memcpy(dst, &fcycles, sizeof(long)); dst += sizeof(long);
   memcpy(dst, &ticksRunning, sizeof(long)); dst += sizeof(long);


   memcpy(dst, &alg_curr_x,    sizeof(double)); dst += sizeof(double);
   memcpy(dst, &alg_curr_y,    sizeof(double)); dst += sizeof(double);
   memcpy(dst, &supplyVoltage, sizeof(double)); dst += sizeof(double);
   memcpy(dst, &capacitorFarad, sizeof(double)); dst += sizeof(double);
   memcpy(dst, &currentVoltage, sizeof(double)); dst += sizeof(double);
   memcpy(dst, &timeConstant, sizeof(double)); dst += sizeof(double);
   memcpy(dst, &VECTREX_CYCLE_TIME, sizeof(double)); dst += sizeof(double);
   memcpy(dst, &percentageDifChangePerCycle, sizeof(double)); dst += sizeof(double);
   
   memcpy(dst, &timerItemArray, sizeof(TimerItem) *MAX_TIMER); dst += sizeof(TimerItem) *MAX_TIMER;
   *dst = alg_vector_color;

   return 1;
}
int vecx_deserialize(char* dst, int size)
{
   if (size < vecx_statesz())
      return 0;

   e6809_deserialize(dst);
   dst += e6809_statesz();
   e8910_deserialize(dst);
   dst += e8910_statesz();

   memcpy(vecx_ram, dst, 1024);
   dst += 1024;
   memcpy(&snd_select,                  dst, sizeof(int)); dst += sizeof(int); 
   memcpy(&via_ora,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_orb,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_ddra,                    dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_ddrb,                    dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_t1on,                    dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_t1int,                   dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_t1c,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_t1ll,                    dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_t1lh,                    dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_t1pb7,                   dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_t2on,                    dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_t2int,                   dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_t2c,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_t2ll,                    dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_sr,                      dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_srb,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_src,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_srclk,                   dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_acr,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_pcr,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_ifr,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_ier,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&old_via_ca1,                 dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_ca1,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_ca2,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_cb2h,                    dst, sizeof(int)); dst += sizeof(int);
   memcpy(&via_cb2s,                    dst, sizeof(int)); dst += sizeof(int);
   memcpy(&alg_rsh,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&alg_xsh,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&alg_ysh,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&alg_zsh,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&alg_jch0,                    dst, sizeof(int)); dst += sizeof(int);
   memcpy(&alg_jch1,                    dst, sizeof(int)); dst += sizeof(int);
   memcpy(&alg_jch2,                    dst, sizeof(int)); dst += sizeof(int);
   memcpy(&alg_jch3,                    dst, sizeof(int)); dst += sizeof(int);
   memcpy(&alg_jsh,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&alg_compare,                 dst, sizeof(int)); dst += sizeof(int);
   memcpy(&alg_vectoring,               dst, sizeof(int)); dst += sizeof(int);
   memcpy(&alg_sel,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&alg_ramping,                 dst, sizeof(int)); dst += sizeof(int);
   memcpy(&alg_DAC,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&alg_ssh,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&alg_vector_speed, 			dst, sizeof(int)); dst += sizeof(int);
   memcpy(&stepsDone,                   dst, sizeof(int)); dst += sizeof(int);
   memcpy(&sig_blank,                   dst, sizeof(int)); dst += sizeof(int);
   memcpy(&alternate,                   dst, sizeof(int)); dst += sizeof(int);
   memcpy(&currentBank,                 dst, sizeof(int)); dst += sizeof(int);
   memcpy(&currentIRQ,                  dst, sizeof(int)); dst += sizeof(int);
   memcpy(&currentPB6,                  dst, sizeof(int)); dst += sizeof(int);
   memcpy(&BANK_MAX,                    dst, sizeof(int)); dst += sizeof(int);
   memcpy(&resistorOhm,                 dst, sizeof(int)); dst += sizeof(int);
   memcpy(&intensityDrift,              dst, sizeof(int)); dst += sizeof(int);
   memcpy(&thisWaitRecal,               dst, sizeof(int)); dst += sizeof(int);
   memcpy(&syncImpulse,                 dst, sizeof(int)); dst += sizeof(int);
   memcpy(&sig_ramp,                    dst, sizeof(int)); dst += sizeof(int);
   memcpy(&sig_zero,                    dst, sizeof(int)); dst += sizeof(int);
   memcpy(&pb6_in,                      dst, sizeof(int)); dst += sizeof(int);
   memcpy(&pb6_out,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&cartlen,                     dst, sizeof(int)); dst += sizeof(int);
   memcpy(&alg_vector_x0,               dst, sizeof(long)); dst += sizeof(long);
   memcpy(&alg_vector_y0,               dst, sizeof(long)); dst += sizeof(long);
   memcpy(&alg_vector_x1,               dst, sizeof(long)); dst += sizeof(long);
   memcpy(&alg_vector_y1,               dst, sizeof(long)); dst += sizeof(long);
   memcpy(&alg_vector_dx,               dst, sizeof(long)); dst += sizeof(long);
   memcpy(&alg_vector_dy,               dst, sizeof(long)); dst += sizeof(long);
   memcpy(&vector_draw_cnt,             dst, sizeof(long)); dst += sizeof(long);
   memcpy(&vector_erse_cnt,             dst, sizeof(long)); dst += sizeof(long);
   memcpy(&lastAddLine,                 dst, sizeof(long)); dst += sizeof(long);
   memcpy(&cyclesRunning,               dst, sizeof(long)); dst += sizeof(long);
   memcpy(&lastWaitRecal,               dst, sizeof(long)); dst += sizeof(long);
   memcpy(&lastSyncCycles,              dst, sizeof(long)); dst += sizeof(long);
   memcpy(&fcycles,                     dst, sizeof(long)); dst += sizeof(long);
   memcpy(&ticksRunning,                dst, sizeof(long)); dst += sizeof(long);
   memcpy(&alg_curr_x,                  dst, sizeof(double)); dst += sizeof(double);
   memcpy(&alg_curr_y,                  dst, sizeof(double)); dst += sizeof(double);
   memcpy(&supplyVoltage,               dst, sizeof(double)); dst += sizeof(double);
   memcpy(&capacitorFarad,              dst, sizeof(double)); dst += sizeof(double);
   memcpy(&currentVoltage,              dst, sizeof(double)); dst += sizeof(double);
   memcpy(&timeConstant,                dst, sizeof(double)); dst += sizeof(double);
   memcpy(&VECTREX_CYCLE_TIME,          dst, sizeof(double)); dst += sizeof(double);
   memcpy(&percentageDifChangePerCycle, dst, sizeof(double)); dst += sizeof(double);
   memcpy(&timerItemArray,              dst, sizeof(TimerItem) *MAX_TIMER); dst += sizeof(TimerItem) *MAX_TIMER;
   alg_vector_color = *dst;

   return 1;
}

// capacitor emulation (one...)
int getIntVoltageValue()
{
	return (int)currentVoltage;
}
double getVoltageValue()
{
	return currentVoltage;
}
double getDigitalValue()
{
	return currentVoltage/5.0*128.0;
}
int getDigitalIntValue()
{
	return (int) (currentVoltage/5.0*128.0);
}
static einline void doStep()
{
	double dif = supplyVoltage - currentVoltage;
	currentVoltage += percentageDifChangePerCycle*dif;
}
// -128 - +127
void setDigitalVoltage(unsigned char v)
{
	if (v<=127)
	{
		supplyVoltage = (((double)v)/127.0)*5.0;
	}
	else
	{
		supplyVoltage = (((((double)v)-256))/128.0)*5.0;
	}
}
#ifdef FLASH_SUPPORT
// dead ugly - well it works...
void checkEraseSequence()
{
	if ((eraseSequenceAddress == 0) && (addressBUS == 0x5555)) eraseSequenceAddress = 1;
	else if ((eraseSequenceAddress == 1) && ((addressBUS == 0x5555) || (addressBUS == 0x2aaa) ))
	{
		if (addressBUS == 0x2aaa) eraseSequenceAddress = 2;
	}
	else if ((eraseSequenceAddress == 2) && ((addressBUS == 0x5555) || (addressBUS == 0x2aaa) ))
	{
		if (addressBUS == 0x2aaa) eraseSequenceAddress = 3;
	}
	else if ((eraseSequenceAddress == 3) && ((addressBUS == 0x5555) || (addressBUS == 0x2aaa) ))
	{
		if (addressBUS == 0x5555) eraseSequenceAddress = 4;
	}
	else if ((eraseSequenceAddress == 4) && ((addressBUS == 0x5555) || (addressBUS == 0x2aaa) ))
	{
		if (addressBUS == 0x2aaa) eraseSequenceAddress = 5; 
	}
	else if ((eraseSequenceAddress == 5) && (addressBUS == 0x2aaa) )
	{
		;
	}
	else if ((eraseSequenceAddress == 5) && (addressBUS != 0x2aaa) )
	{
		eraseAddress = addressBUS;
		eraseSequenceAddress = 6;
	}
	else if ((eraseSequenceAddress == 6) && (addressBUS != eraseAddress) )
	{
		eraseSequenceAddress = 0;
	}
	else eraseSequenceAddress = 0;

	if ((eraseSequenceData == 0) && (dataBUS == 0xaa))
	{
		eraseSequenceData = 1;
	}
	else if ((eraseSequenceData == 1) && ( (dataBUS == 0xaa)||(dataBUS == 0x55) ))
	{
		if (dataBUS == 0x55) eraseSequenceData = 2;
	}
	else if ((eraseSequenceData == 2) && ( (dataBUS == 0x55)||(dataBUS == 0x80) ))
	{
		if (dataBUS == 0x80) eraseSequenceData = 3;
	}
	else if ((eraseSequenceData == 3) && ( (dataBUS == 0xaa)||(dataBUS == 0x80) ))
	{
		if (dataBUS == 0xaa) eraseSequenceData = 4;
	}
	else if ((eraseSequenceData == 4) && ( (dataBUS == 0xaa)||(dataBUS == 0x55) ))
	{
		if (dataBUS == 0x55) eraseSequenceData = 5;
	}
	else if ((eraseSequenceData == 5) && ( (dataBUS == 0x30)||(dataBUS == 0x55) ))
	{
		if (dataBUS == 0x30) 
		{
			eraseSequenceData = 6;
		}
	}
	else if ((eraseSequenceData == 6) &&  (dataBUS == 0x30))
	{
		;
	}
	else eraseSequenceData = 0;

	if ((eraseSequenceAddress == 6) && (eraseSequenceData == 6))
	{
		// log.addLog("Erase sequence ...", INFO);
		eraseSequenceAddress = 0;
		eraseSequenceData = 0;
		int start = eraseAddress & 0xffff000;
		for (int i= start; i<start+4096;i++)
		{
			cart[i+(currentBank *65536)] = 0xff; // 
		}
	}
}
void checkWriteSequence()
{

	if ((writeSequenceAddress == 0) && (addressBUS == 0x5555)) writeSequenceAddress = 1;
	else if ((writeSequenceAddress == 1) && ((addressBUS == 0x5555) || (addressBUS == 0x2aaa) ))
	{
		if (addressBUS == 0x2aaa) writeSequenceAddress = 2;
	}
	else if ((writeSequenceAddress == 2) && ((addressBUS == 0x5555) || (addressBUS == 0x2aaa) ))
	{
		if (addressBUS == 0x5555) writeSequenceAddress = 3;
	}
	else if ((writeSequenceAddress == 3) && (addressBUS == 0x5555) )
	{
		; 
	}
	else if ((writeSequenceData >= 3) && (writeSequenceAddress == 3)  )
	{
		writeSequenceAddress=4;
	}
	else if ((writeSequenceData >= 3) && (writeSequenceAddress == 4)  )
	{
	}
	else writeSequenceAddress = 0;

	if ((writeSequenceData == 0) && (dataBUS == 0xaa))
	{
		writeSequenceData = 1;
	}
	else if ((writeSequenceData == 1) && ( (dataBUS == 0xaa)||(dataBUS == 0x55) ))
	{
		if (dataBUS == 0x55) writeSequenceData = 2;
	}
	else if ((writeSequenceData == 2) && ( (dataBUS == 0x55)||(dataBUS == 0xA0) ))
	{
		if (dataBUS == 0xA0) writeSequenceData = 3;
	}
	else if ((writeSequenceData == 3) && (dataBUS == 0xA0) )
	{
		;
	}
	else if ((writeSequenceData == 3) && (writeSequenceAddress >= 3)  )
	{
		writeSequenceData = 4;
	}
	else if ((writeSequenceData == 4) && (writeSequenceAddress >= 3)  )
	{
	}
	else writeSequenceData = 0;

	
}
#endif

// assuming
// 64k carts are two banks of 32k
// 256k carts (VB) are 4 banks of 64k
void setBank()
{
	currentBank = 0;
	if (BANK_MAX == 1) return;
	if (BANK_MAX >= 2)
	{
		if (currentPB6) currentBank++;
	}
	if (BANK_MAX >= 4)
	{
		if (currentIRQ) currentBank+=2;
	}
}
void setPB6FromVectrex(int tobe_via_orb, int  tobe_via_ddrb, int orbInitiated)
{
	if (BANK_MAX <= 1) return;

	int npb6 = tobe_via_orb & tobe_via_ddrb & 0x40; // all output (0x40)
	if ((tobe_via_ddrb & 0x40) == 0x00)  npb6 = npb6 | 0x40; // all input (0x40)
	pb6_out = npb6;
	currentPB6 = (npb6 != 0);
	setBank();
}
void setPB6FromExternal(int b)
{
	if (b)
		pb6_in = 0x40;
	else
		pb6_in = 0x00;

}
	
void setIRQFromVectrex(int irq)
{
	if (BANK_MAX <= 2) return;
	currentIRQ = !irq;
	setBank();
}

static einline void int_update ()
{
	if ( (((via_ifr & 0x7f) & (via_ier & 0x7f))) != 0   ) 
	{
		via_ifr |= 0x80;
	} 
	else 
	{
		via_ifr &= 0x7f;
	}
	setIRQFromVectrex(((via_ifr&0x80) !=0 ));
}

void doCheckRamp(int fromOrbWrite)
{
	if (!fromOrbWrite)
	{
		if ((via_acr & 0x80)!=0) 
		{
			if (via_t1pb7==0)
				timerAddItem(via_t1pb7, &sig_ramp, TIMER_RAMP_CHANGE);
			else
				timerAddItem(via_t1pb7, &sig_ramp, TIMER_RAMP_OFF_CHANGE);
		} 
	}
	else
	{
		if ((via_acr & 0x80)==0) 
		{
			if ((via_orb & 0x80) == 0)
				timerAddItem(via_orb & 0x80 , &sig_ramp, TIMER_RAMP_CHANGE);
			else
				timerAddItem(via_orb & 0x80, &sig_ramp, TIMER_RAMP_OFF_CHANGE);
		}
	}
}

/* update the various analog values when orb is written. */
void doCheckMultiplexer()
{
   /* compare the current joystick direction with a reference */
	switch (via_orb & 0x06) 
	{
		case 0x00:
			alg_jsh = makeSigned(alg_jch0); 
			break;
		case 0x02:
			alg_jsh = makeSigned(alg_jch1); 
			break;
		case 0x04:
			alg_jsh = makeSigned(alg_jch2); 
			break;
		case 0x06:
			alg_jsh = makeSigned(alg_jch3); 
			break;
	}                
	// 0-255, 128 middle
   if ((makeUnsigned(alg_jsh)) > ((via_ora&0xff)^0x80))
      alg_compare = 0x20;
   else
      alg_compare = 0;

	if ((via_orb & 0x01) != 0) return;
	
	/* MUX has been enabled, state changed! */
	switch (alg_sel & 0x06) 
	{
		case 0x00:
			/* demultiplexor is on */
			timerAddItem(alg_DAC, &alg_ysh, TIMER_MUX_Y_CHANGE);
			break;
		case 0x02:
			/* demultiplexor is on */
			timerAddItem(alg_DAC, 0, TIMER_MUX_R_CHANGE);
			break;
		case 0x04:
			/* demultiplexor is on */
			timerAddItem(alg_DAC , &alg_zsh, TIMER_MUX_Z_CHANGE);
			intensityDrift = 0;
			break;
		case 0x06:
			/* sound output line */
			timerAddItem(alg_DAC , &alg_ssh, TIMER_MUX_S_CHANGE);
			break;
			
	}
}


void initTimerArray()
{
	for (int i=0;i<MAX_TIMER;i++)
	{
		timerItemArray[i].countDown = 0;
		timerItemArray[i].valueToSet = 0;
		timerItemArray[i].whereToSet = 0;
		timerItemArray[i].type = TIMER_ACTION_NONE;
		timerItemArray[i].active = 0;
	}
}
//return 1 on success
//return 0 on fail
int removeTimer(TimerItem *t)
{
	for (int i=0;i<MAX_TIMER;i++)
	{
		if (&timerItemArray[i] == t)
		{
			timerItemArray[i].active = 0;
			return 1;
		}
	}
	return 0;
}

void timerAddItem(int value, void *destination, int t)
{
	for (int i=0;i<MAX_TIMER;i++)
	{
		if (timerItemArray[i].active == 0)
		{
			timerItemArray[i].active = 1;
			timerItemArray[i].valueToSet = value&0xff;
			timerItemArray[i].whereToSet = destination;
			timerItemArray[i].type = t; 
			timerItemArray[i].countDown = DELAYS[(t&0xff)];
			return;
		}
	}
	printf("NOT ENOUGH TIMERS! PANIC!!!\n");
	exit(3);
}

void timerDoStep()
{
	ticksRunning++;

	for (int i=0;i<MAX_TIMER;i++)
	{
		if (timerItemArray[i].active == 0) continue;
		timerItemArray[i].countDown--;
		if (timerItemArray[i].countDown <=0)
		{
			if (timerItemArray[i].type == TIMER_SHIFT_READ)
			{
				alternate = 1;
				//lastShiftTriggered = cyclesRunning;
				via_ifr &= 0xfb; /* remove shift register interrupt flag */
				via_srb = 0;
				via_srclk = 1;
				int_update ();
			} 
			else if (timerItemArray[i].type == TIMER_SHIFT_WRITE)
			{
				alternate = 1;
				//via_stalling = 0;
				//lastShiftTriggered = cyclesRunning;
				via_sr = timerItemArray[i].valueToSet;
				via_ifr &= 0xfb; /* remove shift register interrupt flag */
				via_srb = 0;
				via_srclk = 1;
				int_update ();
			}
			else if (timerItemArray[i].type == TIMER_T1)
			{
				via_t1on = 1; /* timer 1 starts running */
				via_t1lh = timerItemArray[i].valueToSet;
				via_t1c = (via_t1lh << 8) | via_t1ll;
				via_ifr &= 0xbf; /* remove timer 1 interrupt flag */
				via_t1int = 1;

				via_t1pb7 = 0;
				doCheckRamp(0);
				int_update ();
			}
			else if (timerItemArray[i].type == TIMER_T2)
			{
				via_t2c = ((timerItemArray[i].valueToSet) << 8) | via_t2ll;
				via_t2c += 0; // hack, it seems vectrex (via) takes two cycles to "process" the setting...
				via_ifr &= 0xdf;
				via_t2on = 1; /* timer 2 starts running */
				via_t2int = 1;
				int_update ();
			}
			
			else if (timerItemArray[i].type == TIMER_MUX_R_CHANGE)
			{
				//noiseCycles = cyclesRunning;
				setDigitalVoltage(timerItemArray[i].valueToSet); 
			}
			else if (timerItemArray[i].whereToSet != 0)   
			{
				if (timerItemArray[i].type == TIMER_RAMP_OFF_CHANGE) 
				{
					// difference of 3 is to much, but we have no "smaller" unit than
					// cycles ticks
					// therefor we calculate a fraction on ticks to be as exact as possible
					// the fraction here is not KNOWN, it is
					// experimented
					// analog curcuits don't really care about cycles...
//					if ((t.whereToSet& 0xff) != (t.valueToSet & 0xff))
//					{
//						rampOffFraction = 1;
//					}
				}
				else if (timerItemArray[i].type == TIMER_RAMP_CHANGE) 
				{
					// difference of 3 is to much, but we have no "smaller" unit than
					// cycles ticks
					// therefor we calculate a fraction on ticks to be as exact as possible
					// the fraction here is not KNOWN, it is
					// experimented
					// analog curcuits don't really care about cycles...
//					if ((t.whereToSet& 0xff) != (t.valueToSet & 0xff))
//					{
//						rampOnFraction = 1;
//					}
				}
/*                    
				// ATTENTION!
				// it looks like MUX SEL has a time - offset
				// but the value that is used to "transport" to the receiving SH
				// is the one, when "timing" expires, not when the timer is
				// set
				// luckily that is ALLWAYS via_ora
				// so we can take it here directly and ignore the value that
				// is passed to timing!
				if (t.type == TIMER_MUX_Y_CHANGE)
				{
					// test above "theory" with Y
					t.whereToSet = via_ora & 0xff;
				}
*/
				if (timerItemArray[i].type != TIMER_MUX_Z_CHANGE) // Z must not be negative
				{
					*timerItemArray[i].whereToSet = makeSigned(timerItemArray[i].valueToSet);
				}
				else
				{
					*timerItemArray[i].whereToSet = timerItemArray[i].valueToSet;
				}

				if (timerItemArray[i].type == TIMER_MUX_SEL_CHANGE)
					doCheckMultiplexer();
			
			}
			timerItemArray[i].active = 0;
		}
	}
}

/* update the snd chips internal registers when via_ora/via_orb changes */
static einline void snd_update(int command)
{
   switch (via_orb & 0x18)
   {
      case 0x00:
         /* the sound chip is disabled */
         break;
      case 0x08:
         /* the sound chip is sending data */
		if (command)
			via_ora = e8910_read(snd_select); // this is for joystick - dummy for now!
         break;
      case 0x10:
         /* the sound chip is recieving data */
		 if (command)
		 {
			 if (snd_select != 14)
			 {
				snd_regs[snd_select] = via_ora;
				e8910_write(snd_select, via_ora);
			 }
		 }

         break;
      case 0x18:
         /* the sound chip is latching an address */

         if ((via_ora & 0xf0) == 0x00)
            snd_select = via_ora & 0x0f;

         break;
   }
	if ((via_orb & 0x07) == 0x06) // SEL == 11 -> Sound, Mux ==0 meaning ON
	{
		// dac is sending data to audio hardware
		// since we are used to do audio in PSG anyway, we send the sampled data there to a "dummy" register
		// data is via_ora
		// dummy register, write directly to audio line buffer in psg emulation!
		e8910_write(255, alg_ssh);
	}
}

unsigned char read8 (unsigned address)
{
   unsigned char data = 0;

   /* rom */
   if ((address & 0xe000) == 0xe000)
      data = rom[address & 0x1fff];
   
   else if ((address & 0xe000) == 0xc000)
   {
      /* ram */
      if (address & 0x800)
         data = vecx_ram[address & 0x3ff];
      else if (address & 0x1000)
      {
         /* io */

         switch (address & 0xf)
         {
            case 0x0:
				/* compare signal is an input so the value does not come from
				 * via_orb.
				 */
				if ((via_acr & 0x80) !=0)
				{
					/* timer 1 has control of bit 7 */
					data = ((via_orb & 0x5f) | alg_compare | via_t1pb7);
				} 
				else 
				{
					/* bit 7 is being driven by via_orb */
					data = ((via_orb & 0xdf) | alg_compare);
				}
				if ((via_ddrb & 0x40) == 0) // pb6 is input
				{
					data = data & (0xff-0x40); // ensure pb6 =0
					data = data | (pb6_in); // ensure pb6 in value
				}
				else
				{
				}
				return data&0xff;
               break;
            case 0x1:
               /* register 1 also performs handshakes if necessary */

               /* if ca2 is in pulse mode or handshake mode, then it
                * goes low whenever ira is read.
                */
               if ((via_pcr & 0x0e) == 0x08)
			   {
                    via_ca2 = 0;
				    timerAddItem(via_ca2,&sig_zero, TIMER_ZERO);
			   }

               via_ifr = via_ifr & (0xff-0x02); //
     		   setIRQFromVectrex(((via_ifr&0x80) !=0 )); // 
               /* fall through */

            case 0xf:
               /* the snd chip is driving port a */
               if ((via_orb & 0x18) == 0x08)
                  data = (unsigned char) snd_regs[snd_select];
               else
                  data = (unsigned char) via_ora;

               break;
            case 0x2:
               data = (unsigned char) via_ddrb;
               break;
            case 0x3:
               data = (unsigned char) via_ddra;
               break;
            case 0x4:
				/* T1 low order counter */
				data = via_t1c;
				via_ifr &= 0xbf; /* remove timer 1 interrupt flag */
//                        via_t1int = 0; // THIS WAS original - and is wrong!
				via_t1int = 1;
				int_update ();
				return data&0xff;
               break;
            case 0x5:
               /* T1 high order counter */
               data = (unsigned char) (via_t1c >> 8);
               break;
            case 0x6:
               /* T1 low order latch */
               data = (unsigned char) via_t1ll;
               break;
            case 0x7:
               /* T1 high order latch */
               data = (unsigned char) via_t1lh;
               break;
            case 0x8:
               /* T2 low order counter */
               data      = (unsigned char) via_t2c;
				via_ifr &= 0xdf; /* remove timer 2 interrupt flag */
				via_t2int = 1;
				int_update ();
               break;
            case 0x9:
               /* T2 high order counter */
               data = (unsigned char) (via_t2c >> 8);
               break;
            case 0xa:
               data = (unsigned char) via_sr&0xff;
               timerAddItem(via_sr, 0, TIMER_SHIFT_READ);
               break;
            case 0xb:
               data = (unsigned char) via_acr;
               break;
            case 0xc:
               data = (unsigned char) via_pcr;
               break;
            case 0xd:
               /* interrupt flag register */

               data = (unsigned char) via_ifr;
               break;
            case 0xe:
               /* interrupt enable register */

               data = (unsigned char) (via_ier | 0x80);
               break;
         }
      }
   }
	// 
	else if( address < 0xc000 )
	{
	   if (BANK_MAX<4) 
		   data = cart[address+(currentBank *32768)] & 0xff; // 
	   else 
		   data = cart[address+(currentBank *65536)] & 0xff; // 
#ifdef FLASH_SUPPORT
	   
		if ((idSequenceData == 3) && (idSequenceAddress == 3)  )
		{
			if ((address%2) == 0)
			{
				flashSupport++;
				return 0xbf;
			}
			else
			{
				flashSupport++;
				return 0xb6; // SST39SF020A
			}
		}
#endif
	}
   else
      data = 0xff;
   return data;
}


void write8 (unsigned address, unsigned char data)
{
   /* rom */
	if ((address & 0xe000) == 0xe000) { }
	else if ((address & 0xe000) == 0xc000)
   {
      /* it is possible for both ram and io to be written at the same! */

      if (address & 0x800)
         vecx_ram[address & 0x3ff] = data;

      if (address & 0x1000)
      {
         switch (address & 0xf)
         {
            case 0x0:
				setPB6FromVectrex(data, via_ddrb, 1); // 
				if ((data & 0x7) != (via_orb & 0x07)) // check if state of mux sel changed
				{
					timerAddItem(data, &alg_sel, TIMER_MUX_SEL_CHANGE);
				}
				via_orb = data;

               snd_update (1);

				if ((via_pcr & 0xe0) == 0x80) 
				{
					/* if cb2 is in pulse mode or handshake mode, then it
					 * goes low whenever orb is written.
					 */
					via_cb2h = 0;
					timerAddItem(via_cb2h, &sig_blank, TIMER_BLANK_ON_CHANGE);
	
				}
				doCheckRamp(1);

               break;
            case 0x1:
               /* register 1 also performs handshakes if necessary */

               /* if ca2 is in pulse mode or handshake mode, then it
                * goes low whenever ora is written.
                */
               if ((via_pcr & 0x0e) == 0x08)
			   {
					via_ca2 = 0;
					timerAddItem(via_ca2,&sig_zero, TIMER_ZERO);
			   }
				via_ifr = via_ifr & (0xff-0x02); // clear ca1 interrupt
				setIRQFromVectrex(((via_ifr&0x80) !=0 ));

               /* fall through */

            case 0xf:
               via_ora = data;
			   alg_DAC = makeSigned(data);


               /* output of port a feeds directly into the dac which then
                * feeds the x axis sample and hold.
                */
				timerAddItem(alg_DAC, &alg_xsh, TIMER_XSH_CHANGE);
				doCheckMultiplexer();
               snd_update (0);

               break;
            case 0x2:
				setPB6FromVectrex(via_orb, data, 0); // 
               via_ddrb = data;
               break;
            case 0x3:
               via_ddra = data;
               break;
            case 0x4:
               /* T1 low order counter */
               via_t1ll = data;

               break;
            case 0x5:
               /* T1 high order counter */
               timerAddItem(data,0, TIMER_T1);
               break;
            case 0x6:
               /* T1 low order latch */

               via_t1ll = data;
               break;
            case 0x7:
               /* T1 high order latch */

               via_t1lh = data;
               break;
            case 0x8:
               /* T2 low order latch */

               via_t2ll = data;
               break;
            case 0x9:
               /* T2 high order latch/counter */
               timerAddItem(data,0, TIMER_T2);
               break;
            case 0xa:
				timerAddItem(data, 0, TIMER_SHIFT_WRITE);
               break;
            case 0xb:
				if ((via_acr & 0x1c) != (data & 0x1c))
				{
					if ((data & 0x1c) == 0) // shift reg is switched off - so take the manual value
					{
					}
					else // use the last shift
					{
						timerAddItem(0, &sig_blank, TIMER_BLANK_ON_CHANGE);
					}
				}
				if ((via_acr & 0xc0) != (data & 0xc0))
				{
					via_acr = data;
					doCheckRamp(!((via_acr&0x80) == 0x80));
				}
				
				via_acr = data;
               break;
            case 0xc:
				via_pcr = data;
				if ((via_pcr & 0x0e) == 0x0c) 
				{
					/* ca2 is outputting low */
					via_ca2 = 0;
					timerAddItem(via_ca2,&sig_zero, TIMER_ZERO);
				} 
				else 
				{
					/* ca2 is disabled or in pulse mode or is
					 * outputting high.
					 */
					via_ca2 = 1;
					timerAddItem(via_ca2,&sig_zero, TIMER_ZERO);
				}
				if ((via_acr & 0x1c) == 0)
				{
					if ((via_pcr & 0xe0) == 0xc0) 
					{
						/* cb2 is outputting low */
						via_cb2h = 0;
						timerAddItem(via_cb2h, &sig_blank, TIMER_BLANK_ON_CHANGE);
					} 
					else if ((via_pcr & 0xe0) == 0xe0) 
					{
						/* cb2 is outputting high */
						via_cb2h = 1;
						timerAddItem(via_cb2h, &sig_blank, TIMER_BLANK_OFF_CHANGE);
					} 
					else 
					{
						/* cb2 is disabled or is in pulse mode or is
						 * outputting high.
						 */
						via_cb2h = 1;
						timerAddItem(via_cb2h, &sig_blank, TIMER_BLANK_OFF_CHANGE);
					}
				}
				break;
            case 0xd:
				/* interrupt flag register */
				via_ifr &= ~(data & 0x7f);
				int_update ();
               break;
            case 0xe:
				/* interrupt enable register */
				if ((data & 0x80) !=0)
				{
					via_ier |= data & 0x7f;
				} 
				else 
				{
					via_ier &= ~(data & 0x7f);
				}
				int_update ();
               break;
         }
      }
   }
   else if (address < 0xC000) 
   { 
		//printf ("Rom write access at: %4X %2X\n", address, data);
		//void writeExtreme(int addr, byte data)
		if ((address&0xff)==0xff) 
		{
			if (data==2) 
			{	
				static int pos = 0;
				static int readLen = 0;
				if (movieBuffer==NULL) 
				{
					FILE *moveFile = NULL;
					// bad apple looks BAD with drift! :-)
					config_drift_x = 0;
					config_drift_y = 0;
					
					char *path = getMoviePath(); // libretro.c
					if (path == NULL)
					{
						//printf("no movie path!\n");
						return;
					}
//printf ("Looking to open Movie: %s\n", path);
					moveFile =fopen(path, "rb");
					if (moveFile == NULL) return;
//printf ("File opened!\n");

					fseek(moveFile, 0L, SEEK_END);
					int len = ftell(moveFile);
//printf ("Size: %i\n", len);
					rewind(moveFile);
					fseek(moveFile, 0, SEEK_SET);



					movieBuffer = malloc(len);
					if (movieBuffer == NULL)
					{
						fclose(moveFile);
						moveFile = NULL;
						return;
					}
//printf ("Buffer allocated!\n");

  readLen = fread(movieBuffer, 1, len, moveFile);
					
//printf ("File read size: %i\n", readLen);
//if (feof(moveFile)) printf ("END OF FILE\n");
//if (ferror(moveFile)) printf ("READ ERROR \n");
  				    fclose(moveFile);
					moveFile = NULL;

					if (readLen != len) 
					{
//printf ("Size Mismatch!\n");
						free(movieBuffer);
						movieBuffer = NULL;
// exit(1);
						return;
					}
				}
				if (readLen<pos+1024+512) pos = 0;
				for (int ii=0; ii< 1024+512;ii++)
				{
//					cart[currentBank][0x4000+ii] = movieBuffer[pos];
					// allways no bankswitch!
					cart[0x4000+ii] = movieBuffer[pos];
					pos++;
				}
//				if (doExtremeOutput)
//					System.out.println("Read 1536 bytes "+String.format("%02X", cart[currentBank][0x4000])+".");
			}
		}
#ifdef FLASH_SUPPORT
		if ((writeSequenceAddress >= 3) && (writeSequenceData >= 3))
		{
			if ((address!=0x5555) && (address!=0x2aaa))
			{
				writeSequenceAddress = 0;
				writeSequenceData = 0;
				if (address>0xffff) return;
				unsigned char oldData = (unsigned char) (cart[address+(currentBank *65536)] & 0xff);

				// only erase of bit is allowed!
				unsigned char newData = (unsigned char) (data & oldData);
				cart[address+(currentBank *65536)] = newData;
//				printf("FLASH write (%i, %4X->%2x)\n", currentBank, address, newData);
				flashcartChanged=1;
			}
		}
#endif		
		
		

	} /* cartridge */
}
void vecx_reset (void)
{
	unsigned r;
	setbuf(stdout, NULL); // easier to debug with printf
	/* ram */

	for (r = 0; r < 1024; r++)
		vecx_ram[r] = r & 0xff;

	for (r = 0; r < 16; r++)
   {
		snd_regs[r] = 0;
		e8910_write(r, 0);
	}

	/* input buttons */

	snd_regs[14] = 0xff;
	e8910_write(14, 0xff);

	snd_select = 0;

	via_ora = 0;
	via_orb = 0;
	via_ddra = 0;
	via_ddrb = 0;
	via_t1on = 0;
	via_t1int = 0;
	via_t1c = 0;
	via_t1ll = 0;
	via_t1lh = 0;
	via_t1pb7 = 0x80;
	via_t2on = 0;
	via_t2int = 0;
	via_t2c = 0;
	via_t2ll = 0;
	via_sr = 0;
	via_srb = 8;
	via_src = 0;
	via_srclk = 0;
	via_acr = 0;
	via_pcr = 0;
	via_ifr = 0;
	via_ier = 0;
	old_via_ca1 = 1;
	via_ca1 = 1;
	via_ca2 = 1;
	via_cb2h = 1;
	via_cb2s = 0;

	alg_rsh = 128;
	alg_xsh = 128;
	alg_ysh = 128;
	alg_zsh = 0;
	alg_jch0 = 128;
	alg_jch1 = 128;
	alg_jch2 = 128;
	alg_jch3 = 128;
	alg_jsh = 128;

	alg_compare = 0; /* check this */

	alg_curr_x = ALG_MAX_X / 2;
	alg_curr_y = ALG_MAX_Y / 2;

	alg_vectoring = 0;

	vector_draw_cnt = 0;
	vector_erse_cnt = 0;
	vectors_draw = vectors_set;
	vectors_erse = vectors_set + VECTOR_CNT;


	currentVoltage=0;
	timeConstant = resistorOhm*capacitorFarad;

	VECTREX_CYCLE_TIME = (double) 1.0/1500000.0;
	percentageDifChangePerCycle = exp(-VECTREX_CYCLE_TIME/timeConstant);
	stepsDone = 0;
	
	alternate = 0; // 
	setDigitalVoltage(0x80); // 

	fcycles = FCYCLES_INIT;

	e6809_read8 = read8;
	e6809_write8 = write8;

	e6809_reset ();
	currentPB6 = 1;
	currentIRQ = 1;
	
	BANK_MAX = 1;
	{
	  currentBank = 0; // 
	  if (cartlen > 50000)
	  {
		BANK_MAX = 2;
		currentBank = 1; // 
	  }
	  if (cartlen > 100000)
	  {
		BANK_MAX = 4;
		currentBank = 3; // 
	  }
	}
	initTimerArray();
	
}


/* perform a single cycle worth of via emulation.
 * via_sstep0 is the first postion of the emulation.
 */

static einline void via_sstep0 (void)
{
	int t2shift;
	if (via_t1on!=0) 
	{
		
		via_t1c--;
		if ((via_t1c & 0xffff) == 0xffff) // two cycle "MORE" since in via manual it says timer runs 1,5 cycles to long
		{
			/* counter just rolled over */
			if ((via_acr & 0x40) != 0)
			{
				/* continuous interrupt mode */
				via_ifr |= 0x40;
				int_update ();
				via_t1pb7 = 0x80 - via_t1pb7;
				doCheckRamp(0);
				/* reload counter */
				via_t1c = ((via_t1lh << 8)&0xff00) | (via_t1ll&0xff);
			} 
			else 
			{
				/* one shot mode */
				if (via_t1pb7 != 0x80)
				{
					via_t1pb7 = 0x80;
					doCheckRamp(0);
				}
				else
				{
					via_t1pb7 = 0x80;
				}
				if (via_t1int != 0) 
				{
					via_ifr |= 0x40;
					int_update ();
					via_t1int = 0;
				}
			}
		}
	}

	if ((via_t2on!=0) && (via_acr & 0x20) == 0x00) 
	{
		via_t2c--;
		if ((via_t2c & 0xffff) == 0xffff) // two cycle "MORE" since in via manual it says timer runs 1,5 cycles to long
		{
			/* one shot mode */
			if (via_t2int!=0) 
			{
				via_ifr |= 0x20;
				int_update ();
				via_t2int = 0;
				syncImpulse = 1;
			}
		}
	}

	// shift counter 
	via_src--;
	if ((via_src & 0xff) == 0xff) 
	{
		via_src = via_t2ll;
		if (via_srclk == 3) 
		{
			t2shift = 1;
			via_srclk = 0;
		} 
		else 
		{
			t2shift = 0;
			via_srclk = (via_srclk+1)%4;
		}
	} 
	else 
	{
		t2shift = 0;
	}
	if (via_srb < 8) 
	{
		switch (via_acr & 0x1c) 
		{
			case 0x00:
				// disabled 
				break;
			case 0x04:
				// shift in under control of t2 
				if (t2shift!=0) 
				{
					// shifting in 0s since cb2 is always an output 
					via_sr <<= 1;
					via_srb++;
				}
				break;
			case 0x08:
				// shift in under system clk control 
				via_sr <<= 1;
				via_srb++;
				break;
			case 0x0c:
				// shift in under cb1 control 
				break;
			case 0x10:
				// shift out under t2 control (free run) 
				if (t2shift!=0) 
				{
					via_cb2s = (via_sr >> 7) & 1;
					via_sr <<= 1;
					via_sr |= via_cb2s;

					timerAddItem(via_cb2s, &sig_blank, (via_cb2s==1) ? TIMER_BLANK_OFF_CHANGE : TIMER_BLANK_ON_CHANGE);
				}
				break;
			case 0x14:
				/// shift out under t2 control 
				if (t2shift!=0) 
				{
					via_cb2s = (via_sr >> 7) & 1;
					via_sr <<= 1;
					via_sr |= via_cb2s;
					timerAddItem(via_cb2s, &sig_blank, (via_cb2s==1) ? TIMER_BLANK_OFF_CHANGE : TIMER_BLANK_ON_CHANGE);
					via_srb++;
				}
				break;
			case 0x18:
/*                    
				// shift out under system clock control 
*/                    
				// System Time -> look at hardware manual
				// only every SECOND cycle!
				alternate = !alternate;
				if (alternate)
				{
					via_cb2s = (via_sr >> 7) & 1;
					via_sr <<= 1;
					via_sr |= via_cb2s;
					timerAddItem(via_cb2s, &sig_blank, (via_cb2s==1) ? TIMER_BLANK_OFF_CHANGE : TIMER_BLANK_ON_CHANGE);
					via_srb++;
				}
				break;
			case 0x1c:
				// shift out under cb1 control 
				break;
		}
		
		if (via_srb == 8)
		{
			via_ifr |= 0x04;
			int_update ();
			//lastShift = via_cb2s;
		}
	}
}


/* perform the second part of the via emulation */

static einline void via_sstep1 (void)
{
	if ((via_pcr & 0x0e) == 0x0a) 
	{
		/* if ca2 is in pulse mode, then make sure
		 * it gets restored to '1' after the pulse.
		 */
		via_ca2 = 1;
		timerAddItem(via_ca2, &sig_zero, TIMER_ZERO);
	}

	if ((via_pcr & 0xe0) == 0xa0) 
	{
		/* if cb2 is in pulse mode, then make sure
		 * it gets restored to '1' after the pulse.
		 */
		via_cb2h = 1;
		timerAddItem(via_cb2h, &sig_blank, TIMER_BLANK_OFF_CHANGE);
	}

	// documentation of VIA
	if (via_ca1 !=old_via_ca1)
	{
		if ((via_pcr & 0x01) == 0x01) // interrupt flag is set by transition low to high
		{
			if (via_ca1 != 0)
			{
				via_ifr = via_ifr | 0x02;
				int_update();
			}
		}
		else // ((via_pcr & 0x01) == 0x00) // interrupt flag is set by transition high to low
		{
			if (via_ca1 == 0)
			{
				via_ifr = via_ifr | 0x02;
				int_update();
			}
		}
	}
	old_via_ca1 =via_ca1;// NEW

}

static einline void alg_addline(
      long x0, long y0,
      long x1, long y1, unsigned char color, int speed)
{
   unsigned long key;
   long index;


	if (cyclesRunning-lastAddLine<15)
	{
		float driftXMax = 1+(float)fabs( (cyclesRunning-lastAddLine)*config_drift_x);
		float driftYMax = 1+(float)fabs( (cyclesRunning-lastAddLine)*config_drift_y);

		int xDif = abs(x0-x1);
		int yDif = abs(y0-y1);
		if ((xDif<=driftXMax) && (yDif<=driftYMax))
		{
			lastAddLine = cyclesRunning;
			return; // this is a more or less "immediate" change of to flags (ramp/blank) and should not be TWO lines (or rather a line and a point)
		}
	}


	lastAddLine = cyclesRunning;




   key = (unsigned long) x0;
   key = key * 31 + (unsigned long) y0;
   key = key * 31 + (unsigned long) x1;
   key = key * 31 + (unsigned long) y1;
   key %= VECTOR_HASH;

   /* first check if the line to be drawn is in the current draw list.
    * if it is, then it is not added again.
    */
   index = vector_hash[key];

   if (index >= 0 && index < vector_draw_cnt &&
         x0 == vectors_draw[index].x0 &&
         y0 == vectors_draw[index].y0 &&
         x1 == vectors_draw[index].x1 &&
         y1 == vectors_draw[index].y1) {
      vectors_draw[index].color = color;
   } else {
      /* missed on the draw list, now check if the line to be drawn is in
       * the erase list ... if it is, "invalidate" it on the erase list.
       */

      if (index >= 0 && index < vector_erse_cnt &&
            x0 == vectors_erse[index].x0 &&
            y0 == vectors_erse[index].y0 &&
            x1 == vectors_erse[index].x1 &&
            y1 == vectors_erse[index].y1) {
         vectors_erse[index].color = VECTREX_COLORS;
      }

      vectors_draw[vector_draw_cnt].x0 = x0;
      vectors_draw[vector_draw_cnt].y0 = y0;
      vectors_draw[vector_draw_cnt].x1 = x1;
      vectors_draw[vector_draw_cnt].y1 = y1;
      vectors_draw[vector_draw_cnt].color = color;
      vectors_draw[vector_draw_cnt].speed = speed;
      vector_hash[key] = vector_draw_cnt;


		if (intensityDrift>100000)
		{
		   double degradePercent = (180000000.0-((double)intensityDrift))/180000000.0; // two minutes
		   if (degradePercent<0) degradePercent = 0;
		   vectors_draw[vector_draw_cnt].color = (int)(((double)color)*degradePercent);
		}


      vector_draw_cnt++;
   }
}

/* perform a single cycle worth of analog emulation */

static einline void alg_sstep (void)
{
   long sig_dx=0, sig_dy=0;
   if (((via_orb & 0x01) == 0) && ((alg_sel & 0x06) == 0x02))
		doStep(); // capacitor
	intensityDrift++;
   if (sig_zero == 0)
   {
      /* need to force the current point to the 'orgin' so just
       * calculate distance to origin and use that as dx,dy.
       */
      sig_dx = ALG_MAX_X/2  - alg_curr_x;
      sig_dy = ALG_MAX_Y/2  - alg_curr_y;
   }
	if (sig_ramp== 0) 
	{
		sig_dx += alg_xsh;
		sig_dy += -alg_ysh;
	} 
	else 
	{
	}

   if (alg_vectoring == 0)
   {
      if ((sig_blank == 1 && 
		alg_curr_x >= 0 && alg_curr_x < ALG_MAX_X && 
		alg_curr_y >= 0 && alg_curr_y < ALG_MAX_Y) 
		&& (((alg_zsh &0x80) ==0) &&  ((alg_zsh&0x7f) !=0) ) )
      {

         /* start a new vector */

         alg_vectoring = 1;
         alg_vector_x0 = alg_curr_x + DELAYS[TIMER_BLANK_OFF_CHANGE]*alg_xsh;
         alg_vector_y0 = alg_curr_y + DELAYS[TIMER_BLANK_OFF_CHANGE]*alg_ysh;
         alg_vector_x1 = alg_curr_x;
         alg_vector_y1 = alg_curr_y;
         alg_vector_dx = sig_dx;
         alg_vector_dy = sig_dy;
		 alg_ramping = (sig_ramp== 0);
         alg_vector_color = makeUnsigned(alg_zsh);
// beware side effects of this kind of macro!
#define _MAX(X, Y) (((X) > (Y)) ? (X) : (Y))		 
		 alg_vector_speed = _MAX(abs(alg_xsh), abs(alg_ysh));
#undef _MAX
      }
   }
   else
   {
      /* already drawing a vector ... check if we need to turn it off */

      if ((sig_blank == 0) || ((alg_zsh&0x80) !=0) || ((alg_zsh &0x7f) ==0))
      {
         /* blank just went on, vectoring turns off, and we've got a
          * new line.
          */

         alg_vectoring = 0;

		if (sig_blank == 0)
		{
			alg_addline (alg_vector_x0, 
						 alg_vector_y0, 
						 alg_vector_x1 + DELAYS[TIMER_BLANK_ON_CHANGE]*alg_xsh, 
						 alg_vector_y1 + DELAYS[TIMER_BLANK_ON_CHANGE]*alg_ysh, 
						 alg_vector_color, alg_vector_speed);
		}
		else
			
		{
			alg_addline (alg_vector_x0, 
						 alg_vector_y0, 
						 alg_vector_x1, 
						 alg_vector_y1, 
						 alg_vector_color, alg_vector_speed);
		}
      }
      else if (((sig_dx != alg_vector_dx) && (sig_ramp== 0)) || 
	           ((sig_dy != alg_vector_dy)&& (sig_ramp== 0)) || 
			   (makeUnsigned(alg_zsh) != alg_vector_color) || 
			   ((sig_ramp == 0) != alg_ramping)
			   )
      {
         /* the parameters of the vectoring processing has changed.
          * so end the current line.
          */
//		int rampStartCheck =  ((!alg_ramping) && (sig_ramp== 0));
//		if (!rampStartCheck)
				alg_addline (alg_vector_x0, 
							 alg_vector_y0, 
							 alg_vector_x1, 
							 alg_vector_y1, 
							 alg_vector_color, alg_vector_speed);


         /* we continue vectoring with a new set of parameters if the
          * current point is not out of limits.
          */

         if (alg_curr_x >= 0 && alg_curr_x < ALG_MAX_X &&
               alg_curr_y >= 0 && alg_curr_y < ALG_MAX_Y)
         {
            alg_vector_x0 = alg_curr_x;
            alg_vector_y0 = alg_curr_y;
            alg_vector_x1 = alg_curr_x;
            alg_vector_y1 = alg_curr_y;
#define _MAX(X, Y) (((X) > (Y)) ? (X) : (Y))		 
			alg_vector_speed = _MAX(abs(alg_xsh), abs(alg_ysh));
#undef _MAX

			if (sig_ramp==0)
			{
				alg_vector_dx = alg_xsh;
				alg_vector_dy = -alg_ysh;
			}
			else
			{
				alg_vector_dx = 0;
				alg_vector_dy = 0;
			}
            alg_vector_color = makeUnsigned(alg_zsh);
         }
         else
            alg_vectoring = 0;
      }
		else // alg vectoring == 1, but nothing changed
		{
			if ((sig_dx == 0) && (sig_dy == 0))
			{
				// dot dwell realized with a zero movement and a high speed
				if (alg_vector_speed<128) alg_vector_speed = 128;
				alg_vector_speed += 0x4;
			}
		}
	}
						
   alg_curr_x += sig_dx;
   alg_curr_y += sig_dy;
	if (sig_ramp== 0) 
	{
		alg_curr_x -= getDigitalValue();
		alg_curr_y += getDigitalValue();
	}        
	if (alg_curr_x>100000) alg_curr_x=100000;
	else if (alg_curr_x<-100000) alg_curr_x=-100000;
	if (alg_curr_y>100000) alg_curr_y=100000;
	else if (alg_curr_y<-100000) alg_curr_y=-100000;

	// drift only when not zeroing
	if (sig_zero != 0) 
	{
		// drift only, when not integrating - or?
		if (sig_ramp != 0)
		{
			alg_curr_x-= config_drift_x;
			alg_curr_y-= config_drift_y;
		}
		else
		{
			alg_curr_x-= config_drift_x/5.0;
			alg_curr_y-= config_drift_y/5.0;
		}
	}

   if (alg_vectoring == 1 &&
         alg_curr_x >= 0 && alg_curr_x < ALG_MAX_X &&
         alg_curr_y >= 0 && alg_curr_y < ALG_MAX_Y)
   {
      /* we're vectoring ... current point is still within limits so
       * extend the current vector.
       */

      alg_vector_x1 = alg_curr_x;
      alg_vector_y1 = alg_curr_y;
   }
}

static einline void vecx_intermediateSteps_static(int count)
{
  for (int c = 0; c < count; c++)
  {
	 via_sstep0 ();
	 timerDoStep();
	 alg_sstep ();
     via_sstep1 ();
	 stepsDone++;
     cyclesRunning++;


#ifdef FLASH_SUPPORT

        if ((idSequenceAddress == 0) && (addressBUS == 0x5555)) idSequenceAddress = 1;
        else if ((idSequenceAddress == 1) && ((addressBUS == 0x5555) || (addressBUS == 0x2aaa) ))
        {
            if (addressBUS == 0x2aaa) idSequenceAddress = 2;
        }
        else if ((idSequenceAddress == 2) && ((addressBUS == 0x5555) || (addressBUS == 0x2aaa) ))
        {
            if (addressBUS == 0x5555) idSequenceAddress = 3;
        }
        else if ((idSequenceAddress == 3) && (addressBUS == 0x5555) )
        {
            ; 
        }
        else if ((idSequenceData == 3) && (idSequenceAddress == 3)  )
        {
            ;
        }
        else idSequenceAddress = 0;

        if ((idSequenceData == 0) && (dataBUS == 0xaa))
        {
            idSequenceData = 1;
        }
        else if ((idSequenceData == 1) && ( (dataBUS == 0xaa)||(dataBUS == 0x55) ))
        {
            if (dataBUS == 0x55) idSequenceData = 2;
        }
        else if ((idSequenceData == 2) && ( (dataBUS == 0x55)||(dataBUS == 0x90) ))
        {
            if (dataBUS == 0x90) idSequenceData = 3;
        }
        else if ((idSequenceData == 3) && (dataBUS == 0x90) )
        {
            ;
        }
        else if ((idSequenceData == 3) && (idSequenceAddress == 3)  )
        {
            // log.addLog("Id Sequence on", INFO);
        }
        else idSequenceData = 0;

        if ((idSequenceAddress == 3) && (idSequenceData == 3))
        {
            if (dataBUS==0xf0)
            {
                idSequenceAddress = 0;
                idSequenceData = 0;
                // log.addLog("Id Sequence off", INFO);
            }
        }

        if (flashSupport>1) // watch lines!
        {
            checkEraseSequence();
            checkWriteSequence();
        }
#endif		
  }
}

void vecx_intermediateSteps(int count)
{
  vecx_intermediateSteps_static(count);
}

void vecx_intermediateStepsUC(int count)
{
  for (int c = 0; c < count; c++)
  {
	 via_sstep0 ();
	 timerDoStep();
	 alg_sstep ();
     via_sstep1 ();
     cyclesRunning++;
  }
}

int vecx_emu (long cycles)
{
	extern unsigned reg_pc;
	
   unsigned icycles;
   int ret = 0;

   while (cycles > 0)
   {
	  stepsDone = 0;
      icycles = e6809_sstep (via_ifr & 0x80, 0);
	  vecx_intermediateSteps_static(icycles-stepsDone);
      if (reg_pc == 0xf1a2) thisWaitRecal = 1;

      cycles -= (long) icycles;
      fcycles -= (long) icycles;
	  
		int doSync = 0;
		if (config_autoSync)
		{
			if (syncImpulse)
			{
				// some carts use T2 for other timing (like digital output), these timers are "realy" small compared to 50 Hz
				if (cyclesRunning - lastSyncCycles < 20000) // do not trust T2 timers which are to lo!
				{
					lastSyncCycles = cyclesRunning;
					syncImpulse = 0;
				}
			}
			if (syncImpulse)
			{
				// this check evens out some peaks above the 3000cycle range
				if (cyclesRunning - lastWaitRecal < 100000)
				{
					if (thisWaitRecal)
					{
						lastSyncCycles = cyclesRunning;
						doSync = 1;
					}
				}
				else
				{
					lastSyncCycles = cyclesRunning;
					doSync = 1;
				}
			}
			else if (fcycles < 0) 
			{
				doSync = 1;
			}
		}
		else
		{
			if (fcycles < 0) 
			{
				doSync = 1;
			}
		}
		
		if (doSync)
		{
			if (thisWaitRecal)
			{
				thisWaitRecal = 0;
				lastWaitRecal = cyclesRunning;
			}                

			syncImpulse = 0;
			fcycles = FCYCLES_INIT;
			 osint_render ();
			 ret = 1;

			 vector_erse_cnt = vector_draw_cnt;
			 vector_draw_cnt = 0;

			 vector_t *tmp = vectors_erse;
			 vectors_erse = vectors_draw;
			 vectors_draw = tmp;
		}	  
	  
   }
   return ret;
}
