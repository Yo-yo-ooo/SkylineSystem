#include "cmath.h"

double InvSqrt(double t) 
{
    float x = (float)t;
    float xhalf = 0.5f*x;
    int i = *(int*)&x; // get bits for floating VALUE 
    i = 0x5f375a86- (i>>1); // gives initial guess y0
    x = *(float*)&i; // convert bits BACK to float
    x = x*(1.5f-xhalf*x*x); // Newton step, repeating increases accuracy
    x = x*(1.5f-xhalf*x*x); // Newton step, repeating increases accuracy
    x = x*(1.5f-xhalf*x*x); // Newton step, repeating increases accuracy

    return ((double)(1/x));
}

double pi=3.14159265359;
 
 
 double fabs_self(double x){
 if(x>=0){
   x=x;
 }else{
   x=-x;
 }
 return x;
 }
 
 
 double power_self(double x,int k){
	double sum=1;
	double i;
 while (k != 0)
    {
		sum*=x;
		--k;
	}
	return sum;
}
 
double sin(double x)
{
	//****************************
	int counter=0;
	if(x<0){
		counter=1;
	}else{
		counter =0;
	}
	x=fabs_self(x);
	//****************************
 int t,q=1;
 double term,factorial=1.0,sum2,sxm,sum1=0;
 
sum2=x;
      for(t=2;;t++)
      {
           factorial=factorial*t;
             if(t%2!=0)
                   {
                 sum1=sum2;
                 q=q*(-1);
                 sxm=fabs_self(power_self(x,t));
                 term=sxm/factorial;
                 sum2=q*term+sum2;
              }
              if(fabs_self(sum2-sum1)<=1e-5)
              break;
        }
          
          if(counter==1){
          	  sum2=-sum2;
          	  return sum2; 
		  }
       
         else{
         	 return sum2;
		 }
        
 }
 
double cos(double x)
{
 
	//********************
	x=fabs_self(x);
	//********************
 int t,q=1;
 double term,factorial=1.0,sum2=1,sxm,sum1=0;
      for(t=2;;t++)
      {
           factorial=factorial*t;
             if(t%2==0)
                   {
                 sum1=sum2;
                 q=q*(-1);
                 sxm=fabs_self(power_self(x,t));
                 term=sxm/factorial;
                 sum2=q*term+sum2;
              }
              if(fabs_self(sum2-sum1)<=1e-5)
              break;
        }
        
          
          	  return sum2; 
	
		 
        
 }

static volatile const double Tiny = 0x1p-1022;

bool isnan(double x) {
    // 先判断 x 是否等于自己
    // 如果 x 等于自己，那么 x 一定不是 NaN，返回 false
    if (x == x) {
        return false;
    } else {
        return true;
    }
}

// Return arctangent(x) given that 2 < x, with the same properties as atan.
static double Tail(double x)
{
	{
		static const double HalfPi = 0x3.243f6a8885a308d313198a2e037ap-1;

		// For large x, generate inexact and return pi/2.
		if (0x1p53 <= x)
			return HalfPi + Tiny;
		if (isnan(x))
			return x - x;
	}

	static const double p03 = -0x1.5555555554A51p-2;
	static const double p05 = +0x1.999999989EBCAp-3;
	static const double p07 = -0x1.249248E1422E3p-3;
	static const double p09 = +0x1.C71C5EDFED480p-4;
	static const double p11 = -0x1.745B7F2D72663p-4;
	static const double p13 = +0x1.3AFD7A0E6EB75p-4;
	static const double p15 = -0x1.104146B1A1AE8p-4;
	static const double p17 = +0x1.D78252FA69C1Cp-5;
	static const double p19 = -0x1.81D33E401836Dp-5;
	static const double p21 = +0x1.007733E06CEB3p-5;
	static const double p23 = -0x1.83DAFDA7BD3FDp-7;

	static const double p000 = +0x1.921FB54442D18p0;
	static const double p001 = +0x1.1A62633145C07p-54;

	double y = 1/x;

	// Square y.
	double y2 = y * y;

	return p001 - ((((((((((((
		+ p23) * y2
		+ p21) * y2
		+ p19) * y2
		+ p17) * y2
		+ p15) * y2
		+ p13) * y2
		+ p11) * y2
		+ p09) * y2
		+ p07) * y2
		+ p05) * y2
		+ p03) * y2 * y + y) + p000;
}


/*	Return arctangent(x) given that 0x1p-27 < |x| <= 1/2, with the same
	properties as atan.
*/
static double atani0(double x)
{
	static const double p03 = -0x1.555555555551Bp-2;
	static const double p05 = +0x1.99999999918D8p-3;
	static const double p07 = -0x1.2492492179CA3p-3;
	static const double p09 = +0x1.C71C7096C2725p-4;
	static const double p11 = -0x1.745CF51795B21p-4;
	static const double p13 = +0x1.3B113F18AC049p-4;
	static const double p15 = -0x1.10F31279EC05Dp-4;
	static const double p17 = +0x1.DFE7B9674AE37p-5;
	static const double p19 = -0x1.A38CF590469ECp-5;
	static const double p21 = +0x1.56CDB5D887934p-5;
	static const double p23 = -0x1.C0EB85F543412p-6;
	static const double p25 = +0x1.4A9F5C4724056p-7;

	// Square x.
	double x2 = x * x;

	return ((((((((((((
		+ p25) * x2
		+ p23) * x2
		+ p21) * x2
		+ p19) * x2
		+ p17) * x2
		+ p15) * x2
		+ p13) * x2
		+ p11) * x2
		+ p09) * x2
		+ p07) * x2
		+ p05) * x2
		+ p03) * x2 * x + x;
}


/*	Return arctangent(x) given that 1/2 < x <= 3/4, with the same properties as
	atan.
*/
static double atani1(double x)
{
	static const double p00 = +0x1.1E00BABDEFED0p-1;
	static const double p01 = +0x1.702E05C0B8155p-1;
	static const double p02 = -0x1.4AF2B78215A1Bp-2;
	static const double p03 = +0x1.5D0B7E9E69054p-6;
	static const double p04 = +0x1.A1247CA5D9475p-4;
	static const double p05 = -0x1.519E110F61B54p-4;
	static const double p06 = +0x1.A759263F377F2p-7;
	static const double p07 = +0x1.094966BE2B531p-5;
	static const double p08 = -0x1.09BC0AB7F914Cp-5;
	static const double p09 = +0x1.FF3B7C531AA4Ap-8;
	static const double p10 = +0x1.950E69DCDD967p-7;
	static const double p11 = -0x1.D88D31ABC3AE5p-7;
	static const double p12 = +0x1.10F3E20F6A2E2p-8;

	double y = x - 0x1.4000000000027p-1;

	return ((((((((((((
		+ p12) * y
		+ p11) * y
		+ p10) * y
		+ p09) * y
		+ p08) * y
		+ p07) * y
		+ p06) * y
		+ p05) * y
		+ p04) * y
		+ p03) * y
		+ p02) * y
		+ p01) * y
		+ p00;
}


/*	Return arctangent(x) given that 3/4 < x <= 1, with the same properties as
	atan.
*/
static double atani2(double x)
{
	static const double p00 = +0x1.700A7C580EA7Ep-01;
	static const double p01 = +0x1.21FB781196AC3p-01;
	static const double p02 = -0x1.1F6A8499714A2p-02;
	static const double p03 = +0x1.41B15E5E8DCD0p-04;
	static const double p04 = +0x1.59BC93F81895Ap-06;
	static const double p05 = -0x1.63B543EFFA4EFp-05;
	static const double p06 = +0x1.C90E92AC8D86Cp-06;
	static const double p07 = -0x1.91F7E2A7A338Fp-08;
	static const double p08 = -0x1.AC1645739E676p-08;
	static const double p09 = +0x1.152311B180E6Cp-07;
	static const double p10 = -0x1.265EF51B17DB7p-08;
	static const double p11 = +0x1.CA7CDE5DE9BD7p-14;

	double y = x - 0x1.c0000000f4213p-1;

	return (((((((((((
		+ p11) * y
		+ p10) * y
		+ p09) * y
		+ p08) * y
		+ p07) * y
		+ p06) * y
		+ p05) * y
		+ p04) * y
		+ p03) * y
		+ p02) * y
		+ p01) * y
		+ p00;
}


/*	Return arctangent(x) given that 1 < x <= 4/3, with the same properties as
	atan.
*/
static double atani3(double x)
{
	static const double p00 = +0x1.B96E5A78C5C40p-01;
	static const double p01 = +0x1.B1B1B1B1B1B3Dp-02;
	static const double p02 = -0x1.AC97826D58470p-03;
	static const double p03 = +0x1.3FD2B9F586A67p-04;
	static const double p04 = -0x1.BC317394714B7p-07;
	static const double p05 = -0x1.2B01FC60CC37Ap-07;
	static const double p06 = +0x1.73A9328786665p-07;
	static const double p07 = -0x1.C0B993A09CE31p-08;
	static const double p08 = +0x1.2FCDACDD6E5B5p-09;
	static const double p09 = +0x1.CBD49DA316282p-13;
	static const double p10 = -0x1.0120E602F6336p-10;
	static const double p11 = +0x1.A89224FF69018p-11;
	static const double p12 = -0x1.883D8959134B3p-12;

	double y = x - 0x1.2aaaaaaaaaa96p0;

	return ((((((((((((
		+ p12) * y
		+ p11) * y
		+ p10) * y
		+ p09) * y
		+ p08) * y
		+ p07) * y
		+ p06) * y
		+ p05) * y
		+ p04) * y
		+ p03) * y
		+ p02) * y
		+ p01) * y
		+ p00;
}


/*	Return arctangent(x) given that 4/3 < x <= 5/3, with the same properties as
	atan.
*/
static double atani4(double x)
{
	static const double p00 = +0x1.F730BD281F69Dp-01;
	static const double p01 = +0x1.3B13B13B13B0Cp-02;
	static const double p02 = -0x1.22D719C06115Ep-03;
	static const double p03 = +0x1.C963C83985742p-05;
	static const double p04 = -0x1.135A0938EC462p-06;
	static const double p05 = +0x1.13A254D6E5B7Cp-09;
	static const double p06 = +0x1.DFAA5E77B7375p-10;
	static const double p07 = -0x1.F4AC1342182D2p-10;
	static const double p08 = +0x1.25BAD4D85CBE1p-10;
	static const double p09 = -0x1.E4EEF429EB680p-12;
	static const double p10 = +0x1.B4E30D1BA3819p-14;
	static const double p11 = +0x1.0280537F097F3p-15;

	double y = x - 0x1.8000000000003p0;

	return (((((((((((
		+ p11) * y
		+ p10) * y
		+ p09) * y
		+ p08) * y
		+ p07) * y
		+ p06) * y
		+ p05) * y
		+ p04) * y
		+ p03) * y
		+ p02) * y
		+ p01) * y
		+ p00;
}


/*	Return arctangent(x) given that 5/3 < x <= 2, with the same properties as
	atan.
*/
static double atani5(double x)
{
	static const double p00 = +0x1.124A85750FB5Cp+00;
	static const double p01 = +0x1.D59AE78C11C49p-03;
	static const double p02 = -0x1.8AD3C44F10DC3p-04;
	static const double p03 = +0x1.2B090AAD5F9DCp-05;
	static const double p04 = -0x1.881EC3D15241Fp-07;
	static const double p05 = +0x1.8CB82A74E0699p-09;
	static const double p06 = -0x1.3182219E21362p-12;
	static const double p07 = -0x1.2B9AD13DB35A8p-12;
	static const double p08 = +0x1.10F884EAC0E0Ap-12;
	static const double p09 = -0x1.3045B70E93129p-13;
	static const double p10 = +0x1.00B6A460AC05Dp-14;

	double y = x - 0x1.d555555461337p0;

	return ((((((((((
		+ p10) * y
		+ p09) * y
		+ p08) * y
		+ p07) * y
		+ p06) * y
		+ p05) * y
		+ p04) * y
		+ p03) * y
		+ p02) * y
		+ p01) * y
		+ p00;
}


// See documentation above.
double atan(double x)
{
	if (x < 0)
		if (x < -1)
			if (x < -5/3.)
				if (x < -2)
					return -Tail(-x);
				else
					return -atani5(-x);
			else
				if (x < -4/3.)
					return -atani4(-x);
				else
					return -atani3(-x);
		else
			if (x < -.5)
				if (x < -.75)
					return -atani2(-x);
				else
					return -atani1(-x);
			else
				if (x < -0x1.d12ed0af1a27fp-27)
					return atani0(x);
				else
					if (x <= -0x1p-1022)
						// Generate inexact and return x.
						return (Tiny + 1) * x;
					else
						if (x == 0)
							return x;
						else
							// Generate underflow and return x.
							return x*Tiny + x;
	else
		if (x <= +1)
			if (x <= +.5)
				if (x <= +0x1.d12ed0af1a27fp-27)
					if (x < +0x1p-1022)
						if (x == 0)
							return x;
						else
							// Generate underflow and return x.
							return x*Tiny + x;
					else
						// Generate inexact and return x.
						return (Tiny + 1) * x;
				else
					return atani0(x);
			else
				if (x <= +.75)
					return +atani1(+x);
				else
					return +atani2(+x);
		else
			if (x <= +5/3.)
				if (x <= +4/3.)
					return +atani3(+x);
				else
					return +atani4(+x);
			else
				if (x <= +2)
					return +atani5(+x);
				else
					return +Tail(+x);
}

double fabs(double num) {
    if (num < 0)
        num = -num;
    return num;
}

float normalized_atan2( float y, float x )
{
    static const uint32_t sign_mask = 0x80000000;
    static const float b = 0.596227f;

    // Extract the sign bits
    uint32_t ux_s  = sign_mask & (uint32_t &)x;
    uint32_t uy_s  = sign_mask & (uint32_t &)y;

    // Determine the quadrant offset
    float q = (float)( ( ~ux_s & uy_s ) >> 29 | ux_s >> 30 ); 

    // Calculate the arctangent in the first quadrant
    float bxy_a = fabs( b * x * y );
    float num = bxy_a + y * y;
    float atan_1q =  num / ( x * x + bxy_a + num );

    // Translate it to the proper quadrant
    uint32_t uatan_2q = (ux_s ^ uy_s) | (uint32_t &)atan_1q;
    return q + (float &)uatan_2q;
} 

double atan2(double x,double y){
    return (double)normalized_atan2((float)y,(float)x);
}