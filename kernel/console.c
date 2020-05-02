// Console input and output.
// Input is from the keyboard or serial port.
// Output is written to the screen and serial port.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"

static void consputc(int);

static int panicked = 0;

static struct {
	struct spinlock lock;
	int locking;
} cons;

static void
printint(int xx, int base, int sign)
{
	static char digits[] = "0123456789abcdef";
	char buf[16];
	int i;
	uint x;

	if(sign && (sign = xx < 0))
		x = -xx;
	else
		x = xx;

	i = 0;
	do{
		buf[i++] = digits[x % base];
	}while((x /= base) != 0);

	if(sign)
		buf[i++] = '-';

	while(--i >= 0)
		consputc(buf[i]);
}

// Print to the console. only understands %d, %x, %p, %s.
void
cprintf(char *fmt, ...)
{
	int i, c, locking;
	uint *argp;
	char *s;

	locking = cons.locking;
	if(locking)
		acquire(&cons.lock);

	if (fmt == 0)
		panic("null fmt");

	argp = (uint*)(void*)(&fmt + 1);
	for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
		if(c != '%'){
			consputc(c);
			continue;
		}
		c = fmt[++i] & 0xff;
		if(c == 0)
			break;
		switch(c){
		case 'd':
			printint(*argp++, 10, 1);
			break;
		case 'x':
		case 'p':
			printint(*argp++, 16, 0);
			break;
		case 's':
			if((s = (char*)*argp++) == 0)
				s = "(null)";
			for(; *s; s++)
				consputc(*s);
			break;
		case '%':
			consputc('%');
			break;
		default:
			// Print unknown % sequence to draw attention.
			consputc('%');
			consputc(c);
			break;
		}
	}

	if(locking)
		release(&cons.lock);
}

void
panic(char *s)
{
	int i;
	uint pcs[10];

	cli();
	cons.locking = 0;
	// use lapiccpunum so that we can call panic from mycpu()
	cprintf("lapicid %d: panic: ", lapicid());
	cprintf(s);
	cprintf("\n");
	getcallerpcs(&s, pcs);
	for(i=0; i<10; i++)
		cprintf(" %p", pcs[i]);
	panicked = 1; // freeze other CPU
	for(;;)
		;
}

#define BACKSPACE 0x100
#define CRTPORT 0x3d4
static ushort *crt = (ushort*)P2V(0xb8000);  // CGA memory
static int tidx = 0; // indeks trenutno aktivnog terminala 
static int tminor = 0; // trenutni minor, ukoliko se razlikuje od tidx samo se ispisuje u pomocni niz, ne na konzolu

static int filter [20] = 
{	//crna   crvena   zelena 	 zuta     plava  ljbicasta    cijan     bela       default  
	// za slova:
	//   0	 	  1	       2	    3		  4	 	     5	      6	       7   8		 9
	0x0000,  0x0400,  0x0200,  0x0e00,   0x0100,    0x0500,  0x0300,  0x0f00,  0,   0x0f00,
	// za pozadinu:
	//  10		 11		  12	   13		 14			15		 16		  17  18		19
	0x0000,  0x4000,  0x2000,  0x6000,   0x1000,	0x5000,  0x3000,  0x0f00,  0,   0x0000
};

static void
cgaputc(int c)
{
	int pos;
	static ushort crt2[6][25*80*sizeof(ushort)];
	static int pos2 [6] = {0}; // indeks do kog smo upisali u svakom od terminala
	static int start [6] = {0}; // da li je aktivan filter za boje
	static int clrs [6] = {0x0700, 0x0700, 0x0700, 0x0700, 0x0700, 0x0700}; // stil boje
	static int t [6] = {0};
	static int pr [6]= {0}; // pr je 1 ako je prethodni karakter bio ESC (033), inace je 0

	// Cursor position: col + 80*row.
	outb(CRTPORT, 14);
	pos = inb(CRTPORT+1) << 8;
	outb(CRTPORT, 15);
	pos |= inb(CRTPORT+1);

	/* ako je vrednost izmedju 151 i 156 (15X) to znaci da je pritisnuto alt+X
	   ovo sam uzeo jer su tu svi kodovi slobodni ()
	*/

	if (c >= 151 && c <= 156) { 

		c -= 151;

		if (tidx != c || tminor != c) {

			while (pos>0)
				crt[--pos] = (' '&0xff) | clrs[tidx];

			tidx = c;
			tminor = c;

			while (pos<pos2[tidx]) {
				crt[pos] = crt2[tidx][pos];
				pos++;
			}

		}
	}
	else

	if (c == 033) {
		pr[tminor] = 1;
	}
	else {
		if(c == '\n') {
			if (tminor == tidx)
				pos += 80 - pos%80;
			pos2[tminor] += 80 - pos2[tminor]%80;
		}
		else if(c == BACKSPACE){
			if(pos > 0 && tminor == tidx) 
				--pos;

			if(pos2[tminor] > 0)
				--pos2[tminor]; 
		} else {
			if (pr[tminor] == 1 && c == '[') {
				start[tminor] = 1;
			}
			else
				if (start[tminor] == 1) {
					// menjaj masku 
					if ('0' <= c && c <= '9')
						t[tminor] = t[tminor]*10 + c-'0';
					if (c == 'm' || c == ';') {
						if (t[tminor] == 0)
							clrs[tminor] = 0x0700;
						else
							if (30 <= t[tminor] && t[tminor] <40) {
								clrs[tminor] &= 0xf0ff;
								clrs[tminor] |= filter[t[tminor] - 30];
							}
							else {
								clrs[tminor] &= 0x0fff;
								clrs[tminor] |= filter[t[tminor] - 30];
							}
						
					t[tminor] = 0;
					if (c == 'm') 
						start[tminor] = 0;
					}
				}
				else {
					if (tminor == tidx)
						crt[pos++] = (c&0xff) | clrs[tidx];
					crt2[tminor][pos2[tminor]++] = (c&0xff) | clrs[tminor];

				}
		}
		pr[tminor] = 0; // prethodni karakter nije esc
	}

	/* stavio sam ovo da bi se ova greska obradila i u slucaju da se desi
	   prilikom ispisa na terminal koji nije aktivan
	*/
	if(pos < 0 || pos > 25*80 || pos2[tminor] < 0 || pos2[tminor] > 25*80)
		panic("pos under/overflow");


	if((pos/80) >= 24){  // Scroll up.
		memmove(crt, crt+80, sizeof(crt[0])*23*80);
		pos -= 80;
		memset(crt+pos, 0, sizeof(crt[0])*(24*80 - pos));
	}

	if((pos2[tminor]/80) >= 24){  // Scroll up.
		memmove(crt2[tminor], crt2[tminor]+80, sizeof(crt2[0][0])*23*80);
		pos2[tminor] -= 80;
		memset(crt2[tminor]+pos2[tminor], 0, sizeof(crt2[0][0])*(24*80 - pos2[tminor]));
	}

	outb(CRTPORT, 14);
	outb(CRTPORT+1, pos>>8);
	outb(CRTPORT, 15);
	outb(CRTPORT+1, pos);
	crt[pos] = ' ' | clrs[tminor];
	crt2[tminor][pos2[tminor]] = ' ' | clrs[tminor];
}

void
consputc(int c)
{
	if(panicked){
		cli();
		for(;;)
			;
	}

	if(c == BACKSPACE){
		uartputc('\b'); uartputc(' '); uartputc('\b');
	} else
		uartputc(c);
	cgaputc(c);
}

#define INPUT_BUF 128
struct {
	char buf[INPUT_BUF];
	uint r;  // Read index
	uint w;  // Write index
	uint e;  // Edit index
} input[6]; // svaki terminal mora da ima svoj input

#define C(x)  ((x)-'@')  // Control-x

void
consoleintr(int (*getc)(void))
{
	int c, doprocdump = 0;


	acquire(&cons.lock);

	while((c = getc()) >= 0){
		// alt+X ne pamtimo nigde, na njega odmah reagujemo
		if (c >= 151 && c <= 156) {
			cgaputc(c);
			continue;
		}
		switch(c){
		case C('P'):  // Process listing.
			// procdump() locks cons.lock indirectly; invoke later
			doprocdump = 1;
			break;
		case C('U'):  // Kill line.
			while(input[tidx].e != input[tidx].w &&
			      input[tidx].buf[(input[tidx].e-1) % INPUT_BUF] != '\n'){
				input[tidx].e--;
				consputc(BACKSPACE);
			}
			break;
		case C('H'): case '\x7f':  // Backspace
			if(input[tidx].e != input[tidx].w){
				input[tidx].e--;
				consputc(BACKSPACE);
			}
			break;
		default:
			if(c != 0 && input[tidx].e-input[tidx].r < INPUT_BUF){
				c = (c == '\r') ? '\n' : c;
				input[tidx].buf[input[tidx].e++ % INPUT_BUF] = c;
				consputc(c);
				if(c == '\n' || c == C('D') || input[tidx].e == input[tidx].r+INPUT_BUF){
					input[tidx].w = input[tidx].e;
					wakeup(&input[tidx].r);
				}
			}
			break;
		}
	}
	release(&cons.lock);
	if(doprocdump) {
		procdump();  // now call procdump() wo. cons.lock held
	}
}


int
consoleread(struct inode *ip, char *dst, int n)
{
	uint target;
	int c;

	int trenutni = ip->minor - 1;

	iunlock(ip);
	target = n;
	acquire(&cons.lock);
	while(n > 0){
		while(input[trenutni].r == input[trenutni].w){
			if(myproc()->killed){
				release(&cons.lock);
				ilock(ip);
				return -1;
			}
			sleep(&input[trenutni].r, &cons.lock);
		}
		c = input[trenutni].buf[input[trenutni].r++ % INPUT_BUF];
		if(c == C('D')){  // EOF
			if(n < target){
				// Save ^D for next time, to make sure
				// caller gets a 0-byte result.
				input[trenutni].r--;
			}
			break;
		}
		*dst++ = c;
		--n;
		if(c == '\n')
			break;
	}
	release(&cons.lock);
	ilock(ip);

	return target - n;
}

int
consolewrite(struct inode *ip, char *buf, int n)
{
	int i;

	tminor = ip->minor - 1;

	iunlock(ip);
	acquire(&cons.lock);
	for(i = 0; i < n; i++)
		consputc(buf[i] & 0xff);
	
	release(&cons.lock);
	ilock(ip);

	/*  u slucaju da ispis ne ide na trenutni terminal tminor treba da 
		da se vrati na trenutni terminal nakon ispisa
	*/
	tminor = tidx;

	return n;
}

void
consoleinit(void)
{
	initlock(&cons.lock, "console");

	devsw[CONSOLE].write = consolewrite;
	devsw[CONSOLE].read = consoleread;
	cons.locking = 1;

	ioapicenable(IRQ_KBD, 0);
}

