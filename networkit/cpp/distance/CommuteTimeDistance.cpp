/*
 * CommuteTimeDistance.cpp
 *
 *  Created on: 29.07.2015
 *      Author: henningm
 */


#include "../../include/networkit/distance/CommuteTimeDistance.hpp"
#include "../../include/networkit/auxiliary/Log.hpp"
#include "../../include/networkit/auxiliary/Timer.hpp"

#include <fstream>
#include <sstream>
#include <math.h>

#include "omp.h"

namespace NetworKit {

CommuteTimeDistance::CommuteTimeDistance(const Graph& G, double tol): Algorithm(), G(G), tol(tol), lamg(1e-5) {
	// main purpose of methd: preparing LAMG

	// construct matrix from graph
	CSRMatrix matrix = CSRMatrix::laplacianMatrix(G);

	// run LAMG setup and measure the time
	Aux::Timer t;
	t.start();
	lamg.setupConnected(matrix);
	t.stop();
	setupTime = t.elapsedMilliseconds();

	DEBUG("done setting up commute time distance");
	hasRun = false;
}

void CommuteTimeDistance::run() {
	count n = G.numberOfNodes();
	distances.resize(n);
	G.forNodes([&](node v){
		distances[v].resize(n, 0.0);
	});

	// temp vector for resetting the solution state
	Vector zeroVector(n, 0.0);

	// set up solution vector and right-hand side
	Vector solution = zeroVector;
	Vector rhs = zeroVector;

	// solve for each pair of nodes
	G.forNodePairs([&](node u, node v){
		// set up right-hand side
		rhs[u] = +1.0;
		rhs[v] = -1.0;
		TRACE("before solve for ", u, " and ", v);

		solution = zeroVector;

		lamg.solve(rhs, solution);
		double diff = fabs(solution[u] - solution[v]);
		distances[u][v] = diff;
		distances[v][u] = diff;
		rhs[u] = 0.0;
		rhs[v] = 0.0;
	});
	exactly = true;
	hasRun = true;
}

uint64_t CommuteTimeDistance::getSetupTime() const {
	return setupTime;
}

void CommuteTimeDistance::runApproximation() {
	count n = G.numberOfNodes();

	// init approximation parameters
	double epsilon2 = tol * tol;
	k = ceil(log2(n)) / epsilon2;

	// entries of random projection matrix
	double randTab[2] = {1/sqrt(k), -1/sqrt(k)};

	solutions.clear();
	solutions.resize(k, Vector(n));

	for (index i = 0; i < k; ++i) {
		Vector rhs(n, 0.0);

		// matrix vector product of q
		// rhs(v) = \sum_e=1 ^m q(e) * B(e, v)
		//        = +/- q(e)
		G.forEdges([&](node u, node v) {
			double r = randTab[Aux::Random::integer(1)];

			if (u < v) {
				rhs[u] += r;
				rhs[v] -= r;
			}
			else {
				rhs[u] -= r;
				rhs[v] += r;
			}
		});


		lamg.solve(rhs, solutions[i]);

	}
	exactly = false;
	hasRun = true;
}

void CommuteTimeDistance::runParallelApproximation() {
	count n = G.numberOfNodes();

	// init approximation parameters
	double epsilon2 = tol * tol;
	k = ceil(log2(n)) / epsilon2;

	// entries of random projection matrix
	double randTab[3] = {1/sqrt(k), -1/sqrt(k)};

	solutions.clear();
	solutions.resize(k, Vector(n));
	std::vector<Vector> rhs(k, Vector(n));

	INFO("Number k of iterations: ", k);
#pragma omp parallel for
	for (omp_index i = 0; i < static_cast<omp_index>(k); ++i) {
		// rhs(v) = \sum_e=1 ^m q(e) * B(e, v)
		//        = +/- q(e)
		G.forEdges([&](node u, node v) {
			double r = randTab[Aux::Random::integer(1)];

			if (u < v) {
				rhs[i][u] += r;
				rhs[i][v] -= r;
			}
			else {
				rhs[i][u] -= r;
				rhs[i][v] += r;
			}
		});
	}
	INFO("Starting the solve phase");
	lamg.parallelSolve(rhs, solutions);
	INFO("Done with the solve phase");

	exactly = false;
	hasRun = true;
}

double CommuteTimeDistance::distance(node u, node v) {
	assureFinished();

	// compute volume
	double volG = 0.0;
	if (! G.isWeighted()) {
		volG = 2.0 * G.numberOfEdges();
	}
	else {
		volG = 2.0 * G.totalEdgeWeight();
	}

	if (exactly) {
		return sqrt(distances[u][v] * volG);
	}
	else {
		double dist = 0.0;
		for (index i = 0; i < k; ++i) {
			double diff = solutions[i][u] - solutions[i][v];
			dist += diff * diff;
		}
		return sqrt(dist * volG);
	}
}

double CommuteTimeDistance::runSinglePair(node u, node v) {
	count n = G.numberOfNodes();
	double dist = 0.0;

	// set up solution vector and status
	Vector solution(n);

	Vector rhs(n, 0.0);
	Vector zeroVector(n, 0.0);
	rhs[u] = +1.0;
	rhs[v] = -1.0;
	// set up right-hand side
	solution = zeroVector;
	lamg.solve(rhs, solution);
	double diff = solution[u] - solution[v];
	dist = fabs(diff);
	return sqrt(dist* G.numberOfEdges());
}

double CommuteTimeDistance::runSingleSource(node u) {
	count n = G.numberOfNodes();
	double dist = 0.0;
	double sum = 0.0;
	Vector zeroVector(n, 0.0);
	// set up solution vector and status
	std::vector<Vector> rhs(n, Vector(n));
	std::vector<Vector> solution(n, Vector(n));
	G.forNodes([&](node i){
		rhs[i] = zeroVector;
		solution[i] = zeroVector;
		rhs[i][u] = +1.0;
		if (i != u) {
			rhs[i][i] = -1.0;
		} else {
			rhs[i][0] = -1.0;
		}
	});

	INFO("rhs.size() = ", rhs.size());
	INFO("solutions.size() = ", solution.size());
	lamg.parallelSolve(rhs, solution);
	G.forNodes([&](node i){
		if (i != u) {
			double diff = solution[i][u] - solution[i][i];
			dist = fabs(diff);
			sum += sqrt(dist);
		}
	});
	return sum * sqrt(G.numberOfEdges());
}

}
