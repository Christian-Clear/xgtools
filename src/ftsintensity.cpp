// Xgtools
// Copyright (C) M. P. Ruffoni 2011-2015
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// ftsintensity : Calibrates the intensity of an FTS spectrum using a response
// function generated by ftsresponse.

// Make sure the GNU Scientific Library (GSL) development package is installed
// on your system, then compile this code using the following command:
//
// g++ ftsintensity.cpp -lgsl -lgslcblas -o ftsintensity
//

#include <cstdlib>
#include <cmath>
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <gsl/gsl_bspline.h>
#include <gsl/gsl_multifit.h>
#include <gsl/gsl_rng.h>
#include <gsl/gsl_randist.h>
#include <gsl/gsl_statistics.h>
#include <vector>
#include <cctype>

using namespace::std;

// ftsresponse version
#define VERSION "1.0"

// Default number of fit coefficients
#define DEFAULT_NUM_COEFFS  200

// Definitions for command line parameters
#define REQUIRED_NUM_ARGS_MODE1 4
#define REQUIRED_NUM_ARGS_MODE2 5
#define ARG_SPECTRUM 1
#define ARG_RESPONSE 2
#define ARG_OUTPUT 3
#define ARG_COEFFS 4

// XGremlin header tags for required variables
#define XMIN_TAG   "wstart"
#define XMAX_TAG   "wstop"
#define DELTAX_TAG "delw"
#define NUM_PTS_TAG "npo"


//------------------------------------------------------------------------------
// showHelp () : Prints syntax help message to the standard output.
//
void showHelp () {
  cout << endl;
  cout << "ftsintensity : Calibrates the intensity of an FTS line spectrum" << endl;
  cout << "---------------------------------------------------------------" << endl;
  cout << "Syntax : ftsintensity <spectrum> <response> <output> [<coeffs>]" << endl << endl;
  cout << "<spectrum>  : An XGremlin line spectrum (do not include the '.dat' extension)." << endl;
  cout << "<response>  : The normalised response function given by ftsresponse." << endl;
  cout << "<output>    : The calibrated line spectrum will be saved here." << endl;
  cout << "<coeffs>    : Number of spline fit coefficients. A larger value will reduce" << endl;
  cout << "              smoothing, allowing higher frequencies to be fitted, but" << endl;
  cout << "              could cause fit instabilities if too high (default "
    << DEFAULT_NUM_COEFFS << ")." << endl << endl;
}


//------------------------------------------------------------------------------
// is_numeric (string) : Determines whether or not argument 1 is a number.
//
bool is_numeric (string a) {
  for (unsigned int i = 0; i < a.length (); i ++) {
    if (!isalnum(a[i]) || isalpha(a[i])) {
      return false;
    }
  }
  return true;
}


//------------------------------------------------------------------------------
// getNumCoefficients (int, char *[]) : Determines how many spline coefficients
// are to be used in the response function fit.
//
size_t getNumCoefficients (int argc, char *argv[]) {
  size_t ncoeffs;
  istringstream iss;
  if (argc == REQUIRED_NUM_ARGS_MODE1) {
    ncoeffs = DEFAULT_NUM_COEFFS;
  } else {
    if (is_numeric (argv[ARG_COEFFS])) {
      iss.str (argv[ARG_COEFFS]);
      iss >> ncoeffs;
      iss.clear ();
    } else {
      cout << "ERROR: Argument " << ARG_COEFFS << " must be a number." << endl;
      return 1;
    }
    if (ncoeffs < 4) {
      cout << "ERROR: The spline fit must contain at least 4 coefficients." << endl;
      return 1;
    }
  }
  cout << "Spline Coefficients : " << ncoeffs << endl;
  return ncoeffs;
}


//------------------------------------------------------------------------------
// getXGremlinHeaderField (ifstream &, string) : Searches the XGremlin header
// file attached to the ifstream at arg1 for the variable specified at arg2.
// If found, it's value is extracted and returned as a double.
//
double getXGremlinHeaderField (ifstream &Header, string FieldName) throw (int) {
  string LineString, NextField;
  double ReturnValue;
  istringstream iss;
 
  // Search the XGremlin header for FieldName
  Header.seekg (ios::beg);
  do {
    getline (Header, LineString);
    iss.str (LineString);
    iss >> NextField;
    iss.clear ();
  } while (NextField != FieldName && !Header.eof ());
  
  // See if the end of file has been reached. If not, extract the variable.
  if (!Header.eof ()) {
    iss.str (LineString.substr (9, 23));
    iss >> ReturnValue;
  } else {
    throw (1);
  }
  
  // Return the extracted XGremlin header variable.
  return ReturnValue;
}

//------------------------------------------------------------------------------
// Main program
//
int main (int argc, char *argv[]) {

  // Check the user's command line input
  if (argc != REQUIRED_NUM_ARGS_MODE1 && argc != REQUIRED_NUM_ARGS_MODE2) {
    showHelp (); 
    return 1;
  }
  
  // Print introductory message to the standard output
  cout << "Normalise an FTS Line Spectrum " << VERSION 
    << " (built " << __DATE__ << ")" << endl;
  cout << "--------------------------------------------------------" << endl;
  cout << "Line Spectrum file  : " << argv [ARG_SPECTRUM] << endl;
  cout << "Response function   : " << argv [ARG_RESPONSE] << endl;
  cout << "Output file         : " << argv [ARG_OUTPUT] << endl;
  
  istringstream iss;
  double xmin, xmax, wstart, wstop, delw;
  int numPts;
  size_t ncoeffs = getNumCoefficients (argc, argv);
  const size_t nbreak = ncoeffs - 2; // nbreak = ncoeffs+2-k = ncoeffs-2 as k=4
  size_t n = 0, i, j;
  gsl_bspline_workspace *bw;
  gsl_vector *B;
  gsl_rng *r;
  gsl_vector *c, *w;
  gsl_vector *x, *y;
  gsl_matrix *X, *cov;
  gsl_multifit_linear_workspace *mw;
  double chisq, Rsq, dof, tss;
  vector <double> xVec, yVec;
  double xi, yi, yerr, ySpline;
  float floatyi, yCal;
  string SpectrumDAT, SpectrumHDR, CalDAT, CalHDR;

  // Variables for file input/output
  SpectrumDAT = argv [ARG_SPECTRUM]; SpectrumDAT += ".dat";
  SpectrumHDR = argv [ARG_SPECTRUM]; SpectrumHDR += ".hdr";
  ifstream spectrum (SpectrumDAT.c_str(), ios::in);
  CalDAT = argv [ARG_OUTPUT]; CalDAT += ".dat";
  CalHDR = argv [ARG_OUTPUT]; CalHDR += ".hdr";
  string LineString, FieldName;

  // Load the spectrum header and extract wstart, wstop, delw, and npo.
  ifstream header (SpectrumHDR.c_str(), ios::in);
  if (header.is_open ()) {
    try {
      wstart = getXGremlinHeaderField (header, XMIN_TAG);
      wstop = getXGremlinHeaderField (header, XMAX_TAG);
      delw = getXGremlinHeaderField (header, DELTAX_TAG);
      numPts = (int) getXGremlinHeaderField (header, NUM_PTS_TAG);
    } catch (int Err) {
      cout << "ERROR: Couldn't load the required XGremlin header data from " << SpectrumHDR << endl;
      return 1;
    }
    cout << "XGremlin variables  : wstart " << wstart << ", wstop " << wstop 
      << ", delw " << delw << ", npo " << numPts << endl;
  } else {
    cout << "ERROR: Unable to open" << SpectrumHDR << endl;
    return 1;
  }

  // Load the normalised response function
  ifstream calData (argv [ARG_RESPONSE], ios::in);
  if (calData.is_open ()) {
    getline (calData, LineString);
    while (!calData.eof ()) {
      n ++;
      iss.str (LineString);
      iss >> xi >> yi;

      xVec.push_back (xi);
      yVec.push_back (yi);
      cout << xi << ", " << yi << endl;
      iss.clear ();

      getline (calData, LineString);
    }
    calData.close ();
  } else {
    cout << "ERROR: Unable to open " << argv [ARG_RESPONSE] << "." << endl;
    return 1;
  }
  xmin = xVec[0];
  xmax = xVec[xVec.size () - 1];
  
  // Check there are sufficient data points in the response function file
  if (n <= ncoeffs) {
    cout << "ERROR: There must be more data points in " << argv [ARG_RESPONSE] 
      << " than spline fit coefficients." << endl;
    return 1;
  }
  
  // Prepare the GSL spline fitting environment
  gsl_rng_env_setup();
  r = gsl_rng_alloc(gsl_rng_default);

  bw = gsl_bspline_alloc(4, nbreak); // allocate a cubic bspline workspace (k=4)
  B = gsl_vector_alloc(ncoeffs);

  x = gsl_vector_alloc(n);
  y = gsl_vector_alloc(n);
  X = gsl_matrix_alloc(n, ncoeffs);
  c = gsl_vector_alloc(ncoeffs);
  w = gsl_vector_alloc(n);
  cov = gsl_matrix_alloc(ncoeffs, ncoeffs);
  mw = gsl_multifit_linear_alloc(n, ncoeffs);
  
  for (i = 0; i < n; i ++) {
    gsl_vector_set (x, i, xVec[i]);
    gsl_vector_set (y, i, yVec[i]);
    gsl_vector_set (w, i, 1.0);
  }

  // use uniform breakpoints between xmin and xmax
  gsl_bspline_knots_uniform(xmin, xmax, bw);

  // construct the fit matrix X
  cout << endl << "Constructing spline ... " << flush;
  for (i = 0; i < n; ++i)
   {
     double xi = gsl_vector_get(x, i);

     // compute B_j(xi) for all j
     gsl_bspline_eval(xi, B, bw);

     // fill in row i of X
     for (j = 0; j < ncoeffs; ++j)
       {
         double Bj = gsl_vector_get(B, j);
         gsl_matrix_set(X, i, j, Bj);
       }
   }

  // do the spline fit
  gsl_multifit_wlinear(X, w, y, c, cov, &chisq, mw);
  dof = n - ncoeffs;
  tss = gsl_stats_wtss(w->data, 1, y->data, 1, y->size);
  Rsq = 1.0 - chisq / tss;
  printf("chisq/dof = %e, Rsq = %f\n", chisq / dof, Rsq);

  // Read in the measured line spectrum
  if (spectrum.is_open ()) {
    ofstream calSpectrum (CalDAT.c_str(), ios::out);
    if (calSpectrum.is_open ()) {
      cout << "Calibrating spectrum ... " << flush;
      for (int i = 0; i < numPts; i ++) {
        spectrum.read ((char*)&floatyi, sizeof (float));

        // Only proceed if xi is within the valid spline interpolation range
        if (i * delw + wstart >= xmin && i * delw + wstart <= xmax) {
          gsl_bspline_eval(i * delw + wstart, B, bw);
          gsl_multifit_linear_est(B, c, cov, &ySpline, &yerr);
           
          // Normalise the line spectrum intensity using the normalised response
          // function
          yCal = floatyi / (float) ySpline;
          calSpectrum.write ((char*)&yCal, sizeof (float));
        } else {
          yCal = 0.0;
          calSpectrum.write ((char*)&yCal, sizeof (float));
        }
      }
      spectrum.close ();
      calSpectrum.close ();
      cout << "done" << endl;
    } else {
      cout << "ERROR: Unable to write to " << CalDAT << endl;
      return 1;
    }
  } else {
    cout << "ERROR: Unable to open " << SpectrumDAT << endl;
    return 1;
  }
  
  // Produce an exact copy of the input header for the calibrated spectrum
  char NextByte;
  ofstream calHeader (CalHDR.c_str(), ios::out);
  header.seekg (ios::beg);
  header.get (NextByte);
  while (!header.eof ()) {
    calHeader.put (NextByte);
    header.get (NextByte);
  }
  header.close ();
  calHeader.close ();
  
  // Free up the GSL environment and terminate the program
  gsl_rng_free(r);
  gsl_bspline_free(bw);
  gsl_vector_free(B);
  gsl_vector_free(x);
  gsl_vector_free(y);
  gsl_matrix_free(X);
  gsl_vector_free(c);
  gsl_vector_free(w);
  gsl_matrix_free(cov);
  gsl_multifit_linear_free(mw);
  return 0;
}
