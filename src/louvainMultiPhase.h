/*
 * louvainMultiPhase.h
 *
 *  Created on: Apr 17, 2017
 *      Author: osu8229
 */

#ifndef LOUVAINMULTIPHASE_H_
#define LOUVAINMULTIPHASE_H_

#include "utilityFunctions.h"
#include "Graph.h"

void runLouvain(Graph &G, unsigned long &originalCommunities, double threshold, double C_threshold);


#endif /* LOUVAINMULTIPHASE_H_ */
