// $Id$
//
// BSpline.cxx: implementation of the BSplineBase class.
//
//////////////////////////////////////////////////////////////////////

//#include <math.h>

#include <vector>
#include <algorithm>
#include <iostream>
#include <ios>
#include <iomanip>

//using namespace std;

#include <tnt.h>
#include <cmat.h>
#include <lu.h>


namespace my {
template <class T> 
inline T abs(const T t) { return (t < 0) ? -t : t; }

template <class T>
inline const T& min (const T& a, const T& b) { return (a < b) ? a : b; }

template <class T>
inline const T& max (const T& a, const T& b) { return (a > b) ? a : b; }
}


/*
 * This is a modified version of R. Pozo's LU_factor template procedure from
 * the Template Numerical Toolkit.  It was modified to limit pivot searching
 * in the case of banded diagonal matrices.  The extra parameter BANDS
 * is the number of bands below the diagonal.
 */
template <class Matrix, class VectorSubscript>
int LU_factor_banded ( Matrix &A, VectorSubscript &indx, int bands)
{
    assert(A.lbound() == 1);                // currently for 1-offset
    assert(indx.lbound() == 1);             // vectors and matrices

    Subscript M = A.num_rows();
    Subscript N = A.num_cols();

    if (M == 0 || N==0) return 0;
    if (indx.dim() != M)
        indx.newsize(M);

    Subscript i=0,j=0,k=0;
    Subscript jp=0;

    Matrix::element_type t;

    Subscript minMN =  (M < N ? M : N) ;        // min(M,N);

    for (j=1; j<= minMN; j++)
    {

        // find pivot in column j and  test for singularity.

        jp = j;
        t = abs(A(j,j));
        for (i=j+1; i<=my::min(j+bands,M); i++)
            if ( abs(A(i,j)) > t)
            {
                jp = i;
                t = abs(A(i,j));
            }

        indx(j) = jp;

        // jp now has the index of maximum element 
        // of column j, below the diagonal

        if ( A(jp,j) == 0 )                 
            return 1;       // factorization failed because of zero pivot


        if (jp != j)            // swap rows j and jp
            for (k=1; k<=N; k++)
            {
                t = A(j,k);
                A(j,k) = A(jp,k);
                A(jp,k) =t;
            }

        if (j<M)                // compute elements j+1:M of jth column
        {
            // note A(j,j), was A(jp,p) previously which was
            // guarranteed not to be zero (Label #1)
            //
            Matrix::element_type recp =  1.0 / A(j,j);

            for (k=j+1; k<=M; k++)
                A(k,j) *= recp;
        }


        if (j < minMN)
        {
            // rank-1 update to trailing submatrix:   E = E - x*y;
            //
            // E is the region A(j+1:M, j+1:N)
            // x is the column vector A(j+1:M,j)
            // y is row vector A(j,j+1:N)

            Subscript ii,jj;

            for (ii=j+1; ii<=M; ii++)
                for (jj=j+1; jj<=N; jj++)
                    A(ii,jj) -= A(ii,j)*A(j,jj);
        }
    }

    return 0;
}   



#include "BSpline.h"

// Our private state structure, which also hides our use
// of TNT for matrices.


struct BSplineBaseP 
{
	C_matrix<float> Q;			// Holds P+Q
	C_matrix<float> LU;			// LU factorization of PQ
	Vector<Subscript> index;
	std::vector<float> X;
	std::vector<float> Nodes;
};


// For now, hardcoding type 1 boundary conditions, 
// which constrains the derivative to zero at the endpoints.
const float BSplineBase::BoundaryConditions[3][4] = 
{ 
	//	0		1		M-1		M
	{	-4,		-1,		-1,		-4 },
	{	0,		1,		1,		0 },
	{	2,		-1,		-1,		2 }
};


//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

#ifdef notdef
BSplineBase::BSplineBase()
{

}
#endif


BSplineBase::~BSplineBase()
{
	Reset();
	delete base;
}


BSplineBase::BSplineBase (const BSplineBase &bb) : 
	K(1), BC(2), base(new BSplineBaseP)
{
	Reset ();
	Copy (bb.base->X.begin(), bb.NX, bb.waveLength);

	xmin = bb.xmin;
	xmax = bb.xmax;
	alpha = bb.alpha;
	DX = bb.DX;
	M = bb.M;
	*base = *bb.base;
}


BSplineBase::BSplineBase (float *x, int nx, float wl) : 
	K(1), BC(2), base(new BSplineBaseP)
{
	setDomain (x, nx, wl);
}


const double BSplineBase::PI = 3.1415927;


// Methods


void
BSplineBase::Reset ()
{
	// Release memory and re-initialize members
	base->X.resize(0);
	waveLength = 0.0;
}



void
BSplineBase::Copy (float *x, int nx, float wl)
{
	if (nx && x)
	{
		// Copy the x array into our storage.
		base->X.resize (nx);
		std::copy (x, x+nx, base->X.begin());
		NX = base->X.size();
	}
	waveLength = wl;
}


void
BSplineBase::setDomain (float * x, int nx, float wl)
{
	// If called while we have an existing array, release it.
	Reset ();
	Copy (x, nx, wl);

	// The Setup() method determines the number of nodes.
	Setup();

	// Now we can calculate alpha and our Q matrix
	alpha = Alpha (waveLength);
	cerr << "Alpha: " << alpha << endl;

	cerr << "Calculating Q..." << endl;
	calculateQ ();
	//cerr << "Array Q after calculation." << endl;
	//cerr << Q;

	cerr << "Calculating P..." << endl;
	addP ();
	if (M < 30)
	{
		cerr << "Array Q after addition of P." << endl;
		cerr << base->Q;
	}

	// Now perform the LU factorization on Q
	cerr << "Beginning LU factoring of P+Q..." << endl;
	factor ();
	cerr << "Done." << endl;
}



/*
 * Calculate the alpha parameter given a wavelength.
 */
float
BSplineBase::Alpha (float wl)
{
	// K is the degree of the derivative constraint: 1, 2, or 3
	float a = (float) (wl / (2 * PI));
	a *= a;				// a^2
	if (k == 2)
		a *= a;			// a^4
	else if (k == 3)
		a *= a * a;		// a^6
	return a;
}


/*
 * Return the correct beta value given the node index.  The value depends
 * on the node index and the current boundary condition type.
 */
inline float
BSplineBase::Beta (int m)
{
	if (m > 1 && m < M-1)
		return 0.0;
	if (m >= M-1)
		m -= M-3;
	assert (0 <= BC && BC <= 2);
	assert (0 <= m && m <= 3);
	return BoundaryConditions[BC][m];
}



/*
 * Given an array of y data points defined over the domain
 * of x data points in this BSplineBase, create a BSpline
 * object which contains the smoothed curve for the y array.
 */
BSpline *
BSplineBase::apply (float *y)
{
	BSpline *spline = new BSpline (*this, y);

	return (spline);
}


/*
 * Evaluate the closed basis function at node m for value x,
 * using the parameters for the current boundary conditions.
 */
float
BSplineBase::Basis (int m, float x)
{
	float y = 0;
	float xm = xmin + (m * DX);
	float z = abs((x - xm) / DX);
	if (z < 2.0)
	{
		z = 2 - z;
		y = 0.25 * (z*z*z);
		z -= 1.0;
		if (z > 0)
			y -= (z*z*z);
	}

	// Boundary conditions, if any, are an additional addend.
	if (m == 0 || m == 1)
		y += Beta(m) * Basis (-1, x);
	else if (m == M-1 || m == M)
		y += Beta(m) * Basis (M+1, x);

	return y;
}



template <class T>
Vector<T>& operator *= (Vector<T> &v, const T mult)
{
	Subscript N = v.dim();
	Vector<T>::iterator vi;
	
	for (vi = v.begin(); vi != v.end(); ++vi)
	{
		*vi *= mult;
	}
	return v;
}



template <class T>
C_matrix<T> &operator += (C_matrix<T> &A,
						  const C_matrix<T> &B)
{
    Subscript M = A.num_rows();
    Subscript N = A.num_cols();

    assert(M==B.num_rows());
    assert(N==B.num_cols());

    Subscript i,j;
    for (i=0; i<M; i++)
        for (j=0; j<N; j++)
            A[i][j] += B[i][j];
	return A;
}



float
BSplineBase::qDelta (int m1, int m2)
/*
 * Return the integral of the product of the basis function derivative restricted
 * to the node domain, 0 to M.
 */
{
	// At present Q is hardcoded for the first derivative
	// filter constraint and the type 1 boundary constraint.

	// These are the products of the first derivative of the
	// normalized basis functions
	// given a distance m nodes apart, qparts[m], 0 <= m <= 3
	// Each column is the integral over each unit domain, -2 to 2
	static const float qparts[4][4] = 
	{
		0.11250f,   0.63750f,   0.63750f,   0.11250f,
		0.00000f,   0.13125f,  -0.54375f,   0.13125f,
		0.00000f,   0.00000f,  -0.22500f,  -0.22500f,
		0.00000f,   0.00000f,   0.00000f,  -0.01875f
	};

	// Simply the sums of each row above, for interior nodes
	// where the integral is not restricted by the domain.
	static const float qinterior[4] =
	{
		1.5f,	-0.28125f,	-0.450f,	-0.01875f
	};

	if (m1 > m2)
		std::swap (m1, m2);

	if (m2 - m1 > 3)
		return 0.0;

	float q = 0.0;
	for (int m = my::max (m1-2,0); m < my::min (m1+2, M); ++m)
		q += qparts[m2-m1][m-m1+2];
	return q * DX * alpha;
}



void
BSplineBase::calculateQ ()
{
#ifdef notdef
	// At present Q is hardcoded for the first derivative
	// filter constraint and the type 1 boundary constraint.

	// These are the products of the first derivative of the
	// normalized basis functions
	// given a distance m nodes apart, q[m], 0 <= m <= 3
	// Each column is the integral over each unit domain, -2 to 2
	static const float qparts[4][4] = 
	{
		0.11250f,   0.63750f,   0.63750f,   0.11250f,
		0.00000f,   0.13125f,  -0.54375f,   0.13125f,
		0.00000f,   0.00000f,  -0.22500f,  -0.22500f,
		0.00000f,   0.00000f,   0.00000f,  -0.01875f
	};

	// Simply the sums of each row above, for interior nodes
	// where the integral is not restricted by the domain.
	static const float qinterior[4] =
	{
		1.5f,	-0.28125f,	-0.450f,	-0.01875f
	};

	Vector<float> qdelta(4, qinterior);
	qdelta *= DX * alpha;
#endif

	C_matrix<float> &Q = base->Q;
	Q.newsize (M+1,M+1);
	Q = 0.0;
	//return;

#ifdef notdef
	// Start by filling in the diagonal where the boundary
	// constraints do not contribute.
	int i;
	for (i = 0; i <= M; ++i)
	{
		Q[i][i] = qdelta[0];
		for (int j = 1; j < 4 && i+j <= M; ++j)
		{
			Q[i][i+j] = qdelta[j];
			Q[i+j][i] = qdelta[j];
		}
	}
#endif
	// First fill in the q values without the boundary constraints.
	int i;
	for (i = 0; i <= M; ++i)
	{
		Q[i][i] = qDelta(i,i);
		for (int j = 1; j < 4 && i+j <= M; ++j)
		{
			Q[i][i+j] = Q[i+j][i] = qDelta (i, i+j);
		}
	}

	cerr.fill(' ');
	cerr.precision(2);
	cerr.setf(std::ios_base::right, std::ios_base::adjustfield);
	cerr.width(5);
	cerr << Q << endl;

	// Now add the boundary constraints:
	// First the upper left corner.
	float b1, b2, q;
	for (i = 0; i <= 1; ++i)
	{
		b1 = Beta(i);
		for (int j = i; j < i+4; ++j)
		{
			b2 = Beta(j);
			assert (j-i >= 0 && j - i < 4);
			q = 0.0;
				qdelta[j-i];
			if (i+1 < 4)
				q += b2*qdelta[i+1];
			if (j+1 < 4)
				q += b1*qdelta[j+1];
			q += b1*b2*qdelta[0];
			Q[j][i] = Q[i][j] = q;
		}
	}

	// Now the lower right
	for (i = M-1; i <= M; ++i)
	{
		b1 = Beta[i - (M-1)];
		for (int j = i - 3; j <= i; ++j)
		{
			b2 = (j >= M - 1) ? Beta[j - (M-1)] : 0;
			q = qdelta[i-j];
			if (M+1-i < 4)
				q += b2*qdelta[M+1-i];
			if (M+1-j < 4)
				q += b1*qdelta[M+1-j];
			q += b1*b2*qdelta[0];
			Q[j][i] = Q[i][j] = q;
		}
	}
}




void
BSplineBase::addP ()
{
	C_matrix<float> P(M+1, M+1, 0.0);
	std::vector<float> &X = base->X;

#ifdef notdef
	for (int m = 0; m <= M; ++m)
	{
		for (int n = 0; n <= M; ++n)
		{
			float sum = 0.0;
			for (int j = 0; j < NX; ++j)
			{
				sum += Basis (m, X[j]) * Basis (n, X[j]) * DX;
			}
			P[m][n] = sum;
		}
	}
#endif

//#ifdef notdef
	int m, n, i;
	// For each data point, sum the product of the nearest, non-zero Basis nodes
	for (i = 0; i < NX; ++i)
	{
		// Which node does this put us in?
		float x = X[i];
		m = (x - xmin) / DX;

		// Loop over the upper triangle of the 5x5 array of basis functions,
		// and add in the products on each side of diagonal.
		for (m = my::max(0, m-2); m <= my::min(M, m+2); ++m)
		{
			float pn;
			float pm = Basis (m, x);
			float sum = pm * pm * DX;
			P[m][m] += sum;
			for (n = m+1; n <= my::min(M, m+3); ++n)
			{
				pm = Basis (m, x);
				pn = Basis (n, x);
				sum = pm * pn * DX;
				P[m][n] += sum;
				P[n][m] += sum;
			}
		}
	}
	//cerr << "Array P: " << P << endl;
//#endif

	base->Q += P;
}



void
BSplineBase::factor ()
{	
	base->index.newsize (M+1);
	base->LU = base->Q;

    if (LU_factor_banded (base->LU, base->index, 3) != 0)
    {
        cerr << "LU_factor() failed." << endl;
		exit (1);
    }
}

	

inline int 
BSplineBase::Ratio (int &ni, float &deltax, float &ratiof,
					float *ratiod)
{
		deltax = (xmax - xmin) / ni;
		ratiof = deltax / waveLength;
		float rd = (float) NX / (float) (ni + 1);
		if (ratiod)
			*ratiod = rd;
		return (rd >= 1.0);
}


/*
 * Return zero if this fails, non-zero otherwise.
 */
int BSplineBase::Setup()
{
	std::vector<float> &X = base->X;
	
	// Find the min and max of the x domain
	xmin = X[0];
	xmax = X[0];

	for (int i = 1; i < NX; ++i)
	{
		if (X[i] < xmin)
			xmin = X[i];
		else if (X[i] > xmax)
			xmax = X[i];
	}

	if (waveLength > xmax - xmin)
	{
		return (0);
	}

	// Minimum acceptable number of nodes per cutoff wavelength
	static const float fmin = 2.0;

	int ni = 9;		// Number of node intervals
	float deltax;
	float ratiof;	// Nodes per wavelength for current deltax
	float ratiod;	// Points per node interval

	do {
		if (! Ratio (++ni, deltax, ratiof))
			return 0;
	}
	while (ratiof > fmin);

	// Tweak the estimates obtained above
	do {
		if (! Ratio (++ni, deltax, ratiof, &ratiod) || 
			  ratiof > 15.0)
		{
			Ratio (--ni, deltax, ratiof);
			break;
		}
	}
	while (ratiof < 4 || ratiod > 2.0);

	// Store the calculations in our state
	M = ni;
	DX = deltax;

	cerr << "Using M node intervals: " << M << " of length DX: " << DX << endl;
	return (1);
}


const float *
BSplineBase::nodes (int *nn)
{
	if (base->Nodes.size() == 0)
	{
		base->Nodes.reserve (M+1);
		for (int i = 0; i <= M; ++i)
		{
			base->Nodes.push_back ( xmin + (i * DX) );
		}
	}

	if (nn)
		*nn = base->Nodes.size();

	assert (base->Nodes.size() == M+1);
	return base->Nodes.begin();
}



//////////////////////////////////////////////////////////////////////
// BSpline Class
//////////////////////////////////////////////////////////////////////

struct BSplineP
{
	std::vector<float> spline;
	Vector<float> A;
};


//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

BSpline::BSpline (BSplineBase &bb, float *y) :
	BSplineBase (bb), s (new BSplineP)
{
	// Given an array of data points over x and its precalculated
	// P+Q matrix, calculate the b vector and solve for the coefficients.

	Vector<float> B(M+1);

	int m, j;
	for (m = 0; m < M+1; ++m)
	{
		float sum = 0.0;
		for (j = 0; j < NX; ++j)
		{
			sum += y[j] * Basis (m, base->X[j]);
		}
		B[m] = sum * DX;
	}

	// Now solve for the A vector.
	s->A = B;
    if (LU_solve (base->LU, base->index, s->A) != 0)
    {
        cerr << "LU_Solve() failed." << endl;
        exit(1);
    }
#ifdef notdef
    cerr << "Solution a for (P+Q)a = b" << endl;
    cerr << " b: " << B << endl;
    cerr << " a: " << s->A << endl;

    cerr << "A*x should be the vector b, ";
    cerr << "residual [s->A*x - b]: " << endl;
	cerr << matmult(base->Q, s->A) - B << endl;
#endif
}


BSpline::~BSpline()
{
	delete s;
}


float BSpline::coefficient (int n)
{
	if (0 <= n && n <= M)
		return s->A[n];
	return 0;
}


float BSpline::evaluate (float x)
{
	float y = 0;
	for (int i = 0; i <= M; ++i)
	{
		y += s->A[i] * Basis (i, x);
	}
	return y;
}


const float * BSpline::curve (int *nx)
{
	std::vector<float> &spline = s->spline;

	// If we already have the curve calculated, don't do it again.
	if (spline.size() == 0)
	{
		spline.reserve (M+1);
		for (int n = 0; n <= M; ++n)
		{
			float x = xmin + (n * DX);
			spline.push_back (evaluate (x));
		}
	}

	if (nx)
		*nx = spline.size();
	return spline.begin();
}
