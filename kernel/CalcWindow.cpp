/* CalcWindow — Scientific integer calculator for GregOS.
   Shunting-yard evaluator: +  -  *  /  %  ^  ( )
   Memory: MC MR M+ M-   Instant: x²  √  1/x
   Custom-drawn color-coded buttons (no GUI::Button).
   Freestanding: no libc, no exceptions.                                     */

#include "../include/CalcWindow.hpp"
#include "../include/WindowManager.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/gfx.h"
#include "../include/keyboard.h"

/* ── Button colors ────────────────────────────────────────────────────────── */
static constexpr unsigned int BC_MEM  = 0x1A4A20u;
static constexpr unsigned int BC_SCI  = 0x1A3440u;
static constexpr unsigned int BC_OP   = 0x1E2A50u;
static constexpr unsigned int BC_NUM  = 0x2A2A2Au;
static constexpr unsigned int BC_CLR  = 0x501818u;
static constexpr unsigned int BC_EQ   = 0x4A2800u;
static constexpr unsigned int BC_MISC = 0x282838u;

/* ── Button table: 5 cols × 6 rows = 30 buttons ─────────────────────────── */
/*   Col:     0             1             2             3             4       */
const CalcWindow::BtnDef CalcWindow::s_buttons[ROWS * COLS] = {
/* R0 */  {"MC",'M',BC_MEM}, {"MR",'R',BC_MEM}, {"M+",'P',BC_MEM}, {"M-",'Q',BC_MEM}, {"%",'%',BC_OP  },
/* R1 */  {"(" ,'(',BC_MISC},{")" ,')',BC_MISC}, {"^", '^',BC_OP  },{"C", 'C',BC_CLR }, {"<",'<',BC_CLR },
/* R2 */  {"1/x",'I',BC_SCI},{"\xFD",'S',BC_SCI},{"\xFB",'V',BC_SCI},{"*",'*',BC_OP  },{"/",'/',BC_OP  },
/* R3 */  {"7", '7',BC_NUM}, {"8", '8',BC_NUM}, {"9", '9',BC_NUM}, {"-", '-',BC_OP  }, {"+",'+'  ,BC_OP},
/* R4 */  {"4", '4',BC_NUM}, {"5", '5',BC_NUM}, {"6", '6',BC_NUM}, {".", '.', BC_MISC},{"+-",'N',BC_MISC},
/* R5 */  {"1", '1',BC_NUM}, {"2", '2',BC_NUM}, {"3", '3',BC_NUM}, {"0", '0',BC_NUM}, {"=", '=',BC_EQ  },
};

/* ── 64-bit helpers (no libgcc) ──────────────────────────────────────────── */

unsigned long long CalcWindow::cw_udiv64(unsigned long long a, unsigned long long b,
                                         unsigned long long* rem)
{
    if (b == 0) { if (rem) *rem = 0; return 0; }
    if (!(a >> 32) && !(b >> 32)) {
        unsigned long qa = (unsigned long)a / (unsigned long)b;
        if (rem) *rem = (unsigned long)a % (unsigned long)b;
        return qa;
    }
    if (b > a) { if (rem) *rem = a; return 0; }
    unsigned long long q = 0, r = 0;
    for (int i = 63; i >= 0; --i) {
        r = (r << 1) | ((a >> (unsigned)i) & 1ULL);
        if (r >= b) { r -= b; q |= (1ULL << (unsigned)i); }
    }
    if (rem) *rem = r;
    return q;
}

long long CalcWindow::cw_div64(long long a, long long b)
{
    if (b == 0) return 0;
    int neg = ((a < 0) ^ (b < 0)) ? 1 : 0;
    unsigned long long ua = a<0 ? (unsigned long long)(-a) : (unsigned long long)a;
    unsigned long long ub = b<0 ? (unsigned long long)(-b) : (unsigned long long)b;
    return neg ? -(long long)cw_udiv64(ua,ub,0) : (long long)cw_udiv64(ua,ub,0);
}

long long CalcWindow::cw_mod64(long long a, long long b)
{
    if (b == 0) return 0;
    unsigned long long ua = a<0 ? (unsigned long long)(-a) : (unsigned long long)a;
    unsigned long long ub = b<0 ? (unsigned long long)(-b) : (unsigned long long)b;
    unsigned long long r = 0;
    cw_udiv64(ua, ub, &r);
    return (a < 0) ? -(long long)r : (long long)r;
}

/* ── Small helpers ────────────────────────────────────────────────────────── */

int CalcWindow::cw_slen(const char* s) { int n=0; while(s[n]) ++n; return n; }

int CalcWindow::cw_itos(long long n, char* buf)
{
    if (n==0) { buf[0]='0'; buf[1]='\0'; return 1; }
    int neg=(n<0); if(neg) n=-n;
    char t[24]; int ti=0;
    while(n>0) { t[ti++]='0'+(int)cw_mod64(n,10); n=cw_div64(n,10); }
    int pos=0;
    if(neg) buf[pos++]='-';
    for(int k=ti-1;k>=0;--k) buf[pos++]=t[k];
    buf[pos]='\0'; return pos;
}

long long CalcWindow::cw_isqrt(long long n)
{
    if (n<=0) return 0;
    long long x=n, y=cw_div64(x+1,2);
    while(y<x) { x=y; y=cw_div64(x+cw_div64(n,x),2); }
    return x;
}

/* ── append_expr ─────────────────────────────────────────────────────────── */

void CalcWindow::append_expr(char c)
{
    int len=cw_slen(m_expr);
    if(len>=MAX_EXPR) return;
    m_expr[len]=c; m_expr[len+1]='\0';
}

/* ── parse_result ────────────────────────────────────────────────────────── */

long long CalcWindow::parse_result() const
{
    long long v=0; int i=0,neg=0;
    if(m_result[i]=='-') { neg=1; i++; }
    while(m_result[i]>='0'&&m_result[i]<='9') v=v*10+(m_result[i++]-'0');
    return neg ? -v : v;
}

/* ── evaluate: shunting-yard → RPN → compute ─────────────────────────────── */

static int sy_prec(char op)
{
    if(op=='+'||op=='-') return 1;
    if(op=='*'||op=='/'||op=='%') return 2;
    if(op=='^') return 3;
    return 0;
}

void CalcWindow::evaluate()
{
    struct Token { int type; long long num; char op; };
    Token output[64]; int out_len=0;
    char  op_stk[32]; int os_top=0;
    bool  last_op=true, error=false;
    const char* s=m_expr;

    for(int i=0; s[i]&&!error; ) {
        while(s[i]==' ') i++;
        char c=s[i]; if(!c) break;

        if(c=='(') {
            if(os_top<32) op_stk[os_top++]='(';
            last_op=true; i++;
        } else if(c==')') {
            while(os_top>0&&op_stk[os_top-1]!='(') {
                if(out_len<64){output[out_len].type=1;output[out_len].op=op_stk[--os_top];out_len++;}
                else --os_top;
            }
            if(os_top>0) --os_top;
            last_op=false; i++;
        } else if(c>='0'&&c<='9') {
            long long v=0;
            while(s[i]>='0'&&s[i]<='9') v=v*10+(s[i++]-'0');
            if(out_len<64){output[out_len].type=0;output[out_len].num=v;out_len++;}
            last_op=false;
        } else if(c=='-'&&last_op) {
            i++;
            long long v=0;
            while(s[i]>='0'&&s[i]<='9') v=v*10+(s[i++]-'0');
            if(out_len<64){output[out_len].type=0;output[out_len].num=-v;out_len++;}
            last_op=false;
        } else if(c=='+'||c=='-'||c=='*'||c=='/'||c=='%'||c=='^') {
            while(os_top>0&&op_stk[os_top-1]!='('&&
                  (sy_prec(op_stk[os_top-1])>sy_prec(c)||
                   (sy_prec(op_stk[os_top-1])==sy_prec(c)&&c!='^'))) {
                if(out_len<64){output[out_len].type=1;output[out_len].op=op_stk[--os_top];out_len++;}
                else --os_top;
            }
            if(os_top<32) op_stk[os_top++]=c;
            last_op=true; i++;
        } else { i++; }
    }
    while(os_top>0) {
        char op=op_stk[--os_top];
        if(op!='('&&out_len<64){output[out_len].type=1;output[out_len].op=op;out_len++;}
    }

    long long stk[32]; int st=0;
    for(int t=0;t<out_len&&!error;t++) {
        if(output[t].type==0) {
            if(st<32) stk[st++]=output[t].num;
        } else {
            if(st<2) { error=true; break; }
            long long b=stk[--st],a=stk[--st],r=0;
            char op=output[t].op;
            if(op=='+') r=a+b;
            else if(op=='-') r=a-b;
            else if(op=='*') r=a*b;
            else if(op=='^') {
                if(b<0||b>62) r=0;
                else { r=1; for(long long e=0;e<b;e++) r*=a; }
            } else if(b==0) { error=true; break; }
            else r=(op=='/')?cw_div64(a,b):cw_mod64(a,b);
            if(st<32) stk[st++]=r;
        }
    }

    if(error||st==0) {
        m_result[0]='E';m_result[1]='r';m_result[2]='r';m_result[3]='\0';
        m_show_result=true; m_expr[0]='\0'; return;
    }

    /* Save "expr=" as history */
    int hi=0,ei=0;
    while(m_expr[ei]&&hi<(int)sizeof(m_history)-3) m_history[hi++]=m_expr[ei++];
    m_history[hi++]='='; m_history[hi]='\0';

    cw_itos(stk[0],m_result);
    m_show_result=true;
}

/* ── instant_fn: x²  √  1/x ─────────────────────────────────────────────── */

void CalcWindow::instant_fn(char k)
{
    if(!m_show_result&&m_expr[0]) evaluate();
    if(!m_show_result||!m_result[0]) return;

    /* Save input string for history */
    char vin[32]; int vi=0;
    while(m_result[vi]&&vi<31) { vin[vi]=m_result[vi]; vi++; } vin[vi]='\0';

    long long v=parse_result(), r=0;
    if(k=='S') {
        r=v*v;
    } else if(k=='V') {
        if(v<0) { m_result[0]='E';m_result[1]='r';m_result[2]='r';m_result[3]='\0'; return; }
        r=cw_isqrt(v);
    } else { /* 'I' = 1/x */
        if(v==0) { m_result[0]='E';m_result[1]='r';m_result[2]='r';m_result[3]='\0'; return; }
        r=cw_div64(1,v);
    }

    /* History: "sqr(vin)=" or "sqrt(vin)=" or "1/(vin)=" */
    const char* pfx=(k=='S')?"sqr(":(k=='V')?"sqrt(":"1/(";
    int hi=0;
    while(*pfx&&hi<(int)sizeof(m_history)-4) m_history[hi++]=*pfx++;
    int si=0;
    while(vin[si]&&hi<(int)sizeof(m_history)-3) m_history[hi++]=vin[si++];
    if(hi<(int)sizeof(m_history)-2) m_history[hi++]=')';
    if(hi<(int)sizeof(m_history)-1) m_history[hi++]='=';
    m_history[hi]='\0';

    cw_itos(r,m_result);
    m_expr[0]='\0';
}

/* ── on_key ───────────────────────────────────────────────────────────────── */

void CalcWindow::on_key(char k)
{
    /* After result: operators continue the chain; digits start fresh.        */
    if(m_show_result) {
        if(k=='+'||k=='-'||k=='*'||k=='/'||k=='%'||k=='^') {
            int ri=0,ei=0;
            while(m_result[ri]&&ei<MAX_EXPR) m_expr[ei++]=m_result[ri++];
            if(ei<MAX_EXPR){m_expr[ei++]=k;m_expr[ei]='\0';}
            m_result[0]='\0'; m_show_result=false; return;
        }
        if(k=='S'||k=='V'||k=='I') { instant_fn(k); return; }
        if(k=='P'||k=='Q') {
            m_memory+=(k=='P')?parse_result():-parse_result(); return;
        }
        /* Keys that don't reset the display */
        if(k!='M'&&k!='R'&&k!='C'&&k!='<'&&k!='=') {
            m_expr[0]='\0'; m_result[0]='\0'; m_history[0]='\0'; m_show_result=false;
        }
    }

    switch(k) {
    case 'C':
        m_expr[0]='\0'; m_result[0]='\0'; m_history[0]='\0'; m_show_result=false;
        break;
    case '<': {
        int len=cw_slen(m_expr);
        if(len>0) { m_expr[len-1]='\0'; m_show_result=false; }
        break;
    }
    case '=':
        if(m_expr[0]) evaluate();
        break;
    case 'S': case 'V': case 'I':
        instant_fn(k);
        break;
    case 'N': {
        if(m_show_result) {
            long long v=-parse_result(); cw_itos(v,m_result);
        } else {
            int len=cw_slen(m_expr); if(!len) break;
            if(m_expr[0]=='-') { for(int i=0;i<len;i++) m_expr[i]=m_expr[i+1]; }
            else if(len<MAX_EXPR) { for(int i=len;i>=0;i--) m_expr[i+1]=m_expr[i]; m_expr[0]='-'; }
        }
        break;
    }
    case 'M': m_memory=0; break;
    case 'R': {
        char mbuf[24]; cw_itos(m_memory,mbuf);
        m_expr[0]='\0'; m_show_result=false; m_result[0]='\0';
        for(int mi=0;mbuf[mi];mi++) append_expr(mbuf[mi]);
        break;
    }
    case 'P': case 'Q': {
        if(!m_show_result&&m_expr[0]) evaluate();
        if(m_show_result) m_memory+=(k=='P')?parse_result():-parse_result();
        break;
    }
    default:
        append_expr(k);
        break;
    }
}

/* ── handle_char ─────────────────────────────────────────────────────────── */

bool CalcWindow::handle_char(int c)
{
    if(c>='0'&&c<='9')              { on_key((char)c); return true; }
    if(c=='+'||c=='-'||c=='*'||c=='%') { on_key((char)c); return true; }
    if(c=='/')                      { on_key('/');     return true; }
    if(c=='('||c==')')              { on_key((char)c); return true; }
    if(c=='^')                      { on_key('^');     return true; }
    if(c=='='||c=='\r'||c=='\n')    { on_key('=');     return true; }
    if(c=='\b'||c==127)             { on_key('<');     return true; }
    if(c=='c'||c=='C')              { on_key('C');     return true; }
    if(c==KEY_ESC)                  { request_close(); return true; }
    return false;
}

/* ── btn_at ──────────────────────────────────────────────────────────────── */

int CalcWindow::btn_at(int px, int py) const
{
    int cx=client_x(),cy=client_y();
    for(int i=0;i<ROWS*COLS;i++) {
        int col=i%COLS,row=i/COLS;
        int bx=cx+BTN_X0+col*(BTN_W+BTN_GAP);
        int by=cy+BTN_Y0+row*(BTN_H+BTN_GAP);
        if(px>=bx&&px<bx+BTN_W&&py>=by&&py<by+BTN_H) return i;
    }
    return -1;
}

/* ── on_event ────────────────────────────────────────────────────────────── */

bool CalcWindow::on_event(const Event& e)
{
    if(e.type==EVT_MOUSE_BUTTON&&(e.mouse.buttons&0x01)) {
        int idx=btn_at(e.mouse.x,e.mouse.y);
        if(idx>=0) { on_key(s_buttons[idx].key); return true; }
    }
    return Window::on_event(e);
}

/* ── draw_button ─────────────────────────────────────────────────────────── */

static unsigned int btn_lighten(unsigned int bc, int d)
{
    unsigned int r=(bc>>16)&0xFF, g=(bc>>8)&0xFF, b=bc&0xFF;
    auto cl=[](unsigned int v,int x)->unsigned int{ int t=(int)v+x; return (unsigned int)(t<0?0:t>255?255:t); };
    return (cl(r,d)<<16)|(cl(g,d)<<8)|cl(b,d);
}

void CalcWindow::draw_button(int idx) const
{
    Graphics& g=Graphics::instance();
    int col=idx%COLS,row=idx/COLS;
    int bx=client_x()+BTN_X0+col*(BTN_W+BTN_GAP);
    int by=client_y()+BTN_Y0+row*(BTN_H+BTN_GAP);

    unsigned int bc=s_buttons[idx].color;
    g.fill_rect(bx,by,BTN_W,BTN_H,bc);
    g.draw_hline(bx,        by,         BTN_W, btn_lighten(bc,70));
    g.draw_vline(bx,        by,         BTN_H, btn_lighten(bc,70));
    g.draw_hline(bx,        by+BTN_H-1, BTN_W, btn_lighten(bc,-50));
    g.draw_vline(bx+BTN_W-1,by,         BTN_H, btn_lighten(bc,-50));

    const char* lbl=s_buttons[idx].label;
    int llen=cw_slen(lbl);
    g.draw_str(bx+(BTN_W-llen*8)/2, by+(BTN_H-8)/2, lbl, 0xEEEEEEu, GFX_TRANSPARENT);
}

/* ── draw ────────────────────────────────────────────────────────────────── */

void CalcWindow::draw()
{
    Window::draw();
    Graphics& g=Graphics::instance();
    int cx=client_x(),cy=client_y(),cw=client_w();

    /* Display panel */
    g.fill_rect(cx,cy,cw,DISP_H,0x141414u);
    g.draw_hline(cx,cy+DISP_H,   cw,Theme::BEVEL_OUTER_DK);
    g.draw_hline(cx,cy+DISP_H+1, cw,Theme::BEVEL_OUTER_LT);

    int max_ch=(cw-12)/8; if(max_ch<1) max_ch=1;

    /* Right-align a string, left-truncate if too long */
    auto rprint=[&](const char* s, int y, unsigned int col) {
        int len=cw_slen(s);
        if(len>max_ch) s+=len-max_ch;
        int dl=cw_slen(s);
        g.draw_str(cx+cw-8-dl*8, y, s, col, GFX_TRANSPARENT);
    };

    if(m_history[0]) rprint(m_history, cy+6,  0x555555u);
    if(m_expr[0])    rprint(m_expr,    cy+28, m_show_result ? 0x444444u : 0xCCCCCCu);
    if(m_show_result&&m_result[0])
                     rprint(m_result,  cy+50, 0xFFDD00u);
    else if(!m_show_result&&!m_expr[0])
                     rprint("0",       cy+50, 0x555555u);

    /* Memory indicator */
    if(m_memory!=0) g.draw_str(cx+6, cy+6, "M", 0x44DD44u, GFX_TRANSPARENT);

    /* Draw all buttons */
    for(int i=0;i<ROWS*COLS;i++) draw_button(i);
}

/* ── open_calc_window ────────────────────────────────────────────────────── */

extern "C" void open_calc_window(void)
{
    auto win=Greg::make_ref<CalcWindow>();
    /* client_w = 4 + 5*52 + 4*4 + 4 = 284
       client_h = 76 + 6*42 + 5*4 + 4 = 352
       window   = 288 × 378 (+ chrome: border=2 each side, title=22) */
    win->setup(240, 50, 288, 378, "Calculatrice", 0x1E1E1Eu);
    WindowManager::instance().add_window(Greg::move(win));
}
