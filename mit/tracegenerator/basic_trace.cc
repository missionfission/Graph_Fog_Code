#include <cstddef>
#include <stdexcept>
#include <stdio.h>
#include <iostream>
#include <fstream>
#include <random>
#include <list>
#include <iomanip>
#include <ctime>    // For time()
#include <cstdlib>  // For srand() and rand()
using namespace std;

typedef tuple<long double, unsigned long, bool> fi_pair_t;
typedef list<fi_pair_t>::iterator list_iterator_t;

// inversion method for bounded Pareto
// uniform sample us, shape a (alpha), lower bound l, upper bound h
double rbpareto(double us, double a, double l, double h)
{
  //  return(pow((pow(l, a) / (us*pow((l/h), a) - us + 1)), (1.0/a)));
  return( l/ pow( 1+us*(pow(l/h,a)-1), 1.0/a) );
}

// double loginv(double us, double a, double l, double h)
// {

//   //  return(pow((pow(l, a) / (us*pow((l/h), a) - us + 1)), (1.0/a)));
//   return( l/ pow( 1+us*(pow(l/h,a)-1), 1.0/a) );
// }

int main (int argc, char* argv[])
{
  // parameters
  if(argc!=8)
  {
    cout << "\n number_of_objects repetition_count mean  stddev lower_bound higher_bound outputname\n";
    return 1;
  }
  // const float h = atoi(argv[8]);
  const long no_objs = atoi(argv[1]);
  const long reps = atoi(argv[2]); // mean for normal distributed
  const double mean = atof(argv[3]); // mean for normal distributed
  const double stddev = atof(argv[4]); // stdev for normal distributed 
  const double lowerb = atof(argv[5]);
  const double higherb = atof(argv[6]);
  const string outputname(argv[7]);

  auto * size = new long[no_objs];
  list<fi_pair_t> reqseq;

  // initialize object sizes
  random_device rd;
  mt19937 rnd_gen (rd ());
  double mean_size=0.0;
  // uniform_real_distribution<> urng(0, 1);
  // for (long i = 0; i < no_objs; i++) {
  //   double us = urng(rnd_gen);
  //   do
  //     {
	// us = urng(rnd_gen);
  //     }
  //   while ((us == 0) || (us == 1));
  //   do
  //   {
  //       size[i]=
  //           rbpareto(us,shape,lowerb,higherb);
  //   }
  //   while (size[i]<lowerb || size[i]>higherb);
  //   mean_size+=size[i];
  // }

 double number;
  std::default_random_engine generator;
  std::normal_distribution<double> distribution(mean,stddev);
  for (int i=0; i<no_objs; ++i) {
    do{
    number = distribution(generator);
        } while ((number<lowerb)||(number>higherb));
    size[i]=long(number);
  }
     
  cout << "finished sizes. mean_size: " << mean_size/static_cast<double>(no_objs) << "\n";

  // initialize req sequence
  for (long i = 0; i < no_objs; i++) {
    const long double rateH = 1/(pow(i+1,0.9));
    exponential_distribution<> iaRandH (rateH);
    long double globalTime = iaRandH(rnd_gen);
    while(globalTime<reps) {
      reqseq.push_back(fi_pair_t(globalTime,i,0));
      globalTime += iaRandH(rnd_gen);
    }
    //    cout << "markov high " << testh/testhc << " number " << testhc << " markov low " << testl/testlc << " number " << testlc << " changed states " << testchange << "\n";
  }
   std::uniform_int_distribution<> dist(0, no_objs);
    for (long i = 0; i < no_objs; i++) {
    const long double rateH = 0.25/(pow(i+1,0.9));
    exponential_distribution<> iaRandH (rateH);
    long double globalTime = iaRandH(rnd_gen);
    long random=dist(rnd_gen);
    while(globalTime<reps) {
      reqseq.push_back(fi_pair_t(globalTime,random,1));
      globalTime += iaRandH(rnd_gen);
    }
    //    cout << "markov high " << testh/testhc << " number " << testhc << " markov low " << testl/testlc << " number " << testlc << " changed states " << testchange << "\n";
  }
  // sort tuples lexicographically, i.e., by time
  cout << "finished raw req sequence.\n";
  reqseq.sort();
  cout << "finished sorting req.\n";

  ofstream outfile;
  outfile.open(outputname);
  outfile<<fixed<<setprecision(0); // turn of scientific notation

  // output request sequence
  list_iterator_t rit;
  for (rit=reqseq.begin(); rit != reqseq.end(); ++rit) {
    outfile << 1000*get<0>(*rit) << " " << get<1>(*rit) << " "<< get<2>(*rit) << " "<< size[get<1>(*rit)] << "\n"; 
  }

  cout << "finished output.\n";
  outfile.close();

  delete size;

  return 0;
}
