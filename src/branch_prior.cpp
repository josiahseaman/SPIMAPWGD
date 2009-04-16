//=============================================================================
//  SPIDIR - branch prior


//#include <gsl/gsl_integration.h>

// c++ headers
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>
#include <time.h>

// spidir headers
#include "common.h"
#include "branch_prior.h"
#include "birthdeath.h"
#include "phylogeny.h"
#include "ExtendArray.h"
#include "seq_likelihood.h"


namespace spidir {


BranchParams NULL_PARAM;




//=============================================================================
// gene rate estimation


// solves x^3 + ax^2 + bx + x = 0 for x
//   in our case there is only one positive root; find it
float maxCubicRoot(float a, float b, float c)
{
    float const accuracy = .001;
    float x = 0.0;
    float y;
    
    // first estimate should be negative    
    assert (x*x*x + a*x*x + b*x + c <= 0);
    
    // increase x until y is positive
    x = .01;
    do {
        x *= 2.0;
        y = x*x*x + a*x*x + b*x + c;
    } while (y < 0);
    
    // binary search to find root
    float min = x / 2.0;
    float max = x;
    
    while (max - min > accuracy) {
        x = (max + min) / 2.0;
        y = x*x*x + a*x*x + b*x + c;
        
        if (y == 0)
            return x;
        else if (y > 0)
            max = x;
        else
            min = x;
    }
    
    return x;
}

int floatcmp(const void *a, const void *b)
{
    float fa = *((float*)a);
    float fb = *((float*)b);
    
    if (fa < fb)
        return -1;
    else if (fa > fb)
        return 1;
    else
        return 0;
}


float mleGeneRate(int count, float *dists, float *means, float *sdevs,
                  float alpha, float beta)
{
    float a, b, c;
    float threshold = 0;
    
    a = (1.0 - alpha) / beta;

    // b = sum(means[i] * lens[i] / sdevs[i]**2
    //         for i in range(len(lens))) / beta    
    // c = - sum(lens[i] ** 2 / sdevs[i] ** 2
    //          for i in range(len(lens))) / beta    
    

    ExtendArray<float> dists2(0, count);
    dists2.extend(dists, count);
    qsort((void*) dists2.get(), dists2.size(), sizeof(float), floatcmp);
    //printFloatArray(dists2, dists2.size());
    //printFloatArray(dists, dists2.size());
    int limit = int(count * .5) + 1;
    if (limit < 4) limit = 4;
    threshold = dists2[limit];
    //printf("threshold %f\n", threshold);
    
    
    b = 0.0;
    c = 0.0;    
    for (int i=0; i<count; i++) {
        if (dists[i] > threshold && sdevs[i] > 0.0001) {
            b += means[i] * dists[i] / (sdevs[i] * sdevs[i]);
            c += dists[i] * dists[i] / (sdevs[i] * sdevs[i]);
        }
    }
    b /= beta;
    c = -c / beta;
    
    return maxCubicRoot(a, b, c);
}


// help set depths for every node
// depth is distance to next subtree root
void estimateGeneRate_helper(Tree *tree, Node *node, float *depths, int *sroots,
                             int *recon, int *events, bool free)
{
    int name = node->name;
    
    if (events[name] == EVENT_SPEC)
        free = false;
    
    if (node != tree->root) {
        int parent = node->parent->name;
        
        if (free) {
            // mark freebranches with -1
            depths[name] = -1;
            sroots[name] = sroots[parent];
        } else {
            if (events[parent] == EVENT_DUP) {
                depths[name] = depths[parent] + node->dist;
                sroots[name] = sroots[parent];
            } else {
                depths[name] = node->dist;
                sroots[name] = recon[parent];
            }
        }
    }
    
    for (int i=0; i<node->nchildren; i++)
        estimateGeneRate_helper(tree, node->children[i], 
                                depths, sroots, recon, events, free);    
}


float estimateGeneRate(Tree *tree, SpeciesTree *stree, 
                       int *recon, int *events, SpidirParams *params)
{
    float *depths = new float [tree->nnodes];
    int *sroots = new int [tree->nnodes];   // species roots
    
    depths[tree->root->name] = 0;
    sroots[tree->root->name] = recon[tree->root->name];
    
    // determine if top subtree is free
    bool free = (recon[tree->root->name] == stree->root->name && 
                 events[tree->root->name] == EVENT_DUP);
    
    estimateGeneRate_helper(tree, tree->root, depths, sroots, 
                            recon, events, free);
    
    //printFloatArray(depths, tree->nnodes);
    //printIntArray(sroots, tree->nnodes);
    
    float *dists = new float [tree->nnodes];
    float *means = new float [tree->nnodes];
    float *sdevs = new float [tree->nnodes];
    
    // make quick access to params
    float *mu = params->sp_alpha;
    float *sigma = params->sp_beta;
    
    int count = 0;
    
    for (int i=0; i<tree->nnodes; i++) {
        if (events[i] != EVENT_DUP && i != tree->root->name) {
            // we are at subtree leaf
            
            // figure out species branches that we cross
            // get total mean and variance of this path            
            float u = 0.0;
            float s2 = 0.0;   
            int snode = recon[i];
            
            // branch is also free if we do not cross any more species
            // don't estimate baserates from extra branches
            if (snode != sroots[i] && depths[i] != -1) {
                while (snode != sroots[i] && snode != stree->root->name) {
                    u += mu[snode];
                    s2 += sigma[snode]*sigma[snode];
                    snode = stree->nodes[snode]->parent->name;
                }
                assert(fabs(s2) > .0000001);
                                
                // save dist and params
                dists[count] = depths[i];
                means[count] = u;
                sdevs[count] = sqrt(s2);
                count++;
            }
        }
    }
    
    
    float generate = (count > 0) ? mleGeneRate(count, dists, means, sdevs, 
                                   params->gene_alpha, params->gene_beta) :
                                   0.0; // catch unusual case
    
    delete [] depths;
    delete [] sroots;
    delete [] dists;
    delete [] means;
    delete [] sdevs;    
    
    return generate;
}


//=============================================================================
// calculate the likelihood of rare events such as gene duplication
float rareEventsLikelihood(Tree *tree, SpeciesTree *stree, int *recon, 
                           int *events, float predupprob, float dupprob, 
                           float lossprob)
{
    if (dupprob < 0.0 || lossprob < 0.0)
        return 0.0;

    ExtendArray<int> recon2(0, tree->nnodes);
    ExtendArray<int> events2(0, tree->nnodes);
    recon2.extend(recon, tree->nnodes);
    events2.extend(events, tree->nnodes);

    int addedNodes = addImpliedSpecNodes(tree, stree, recon2, events2);
    //float logl = birthDeathTreePrior(tree, stree, recon2, events2, 
    //                                 dupprob, lossprob);
    float logl = birthDeathTreeQuickPrior(tree, stree, recon2, events2, 
                                          dupprob, lossprob);
    removeImpliedSpecNodes(tree, addedNodes);
    
    return logl;
}


float rareEventsLikelihood_old(Tree *tree, SpeciesTree *stree, int *recon, 
                           int *events, float predupprob, float dupprob)
{
    float logl = 0.0;
    int sroot = stree->nnodes - 1;
    
    int predups = 0;
    int dups = 0;
    //int losses = countLoss(tree, stree, recon);
    
    // count dups
    for (int i=0; i<tree->nnodes; i++) {
        if (events[i] == EVENT_DUP) {
            if (recon[i] == sroot) {
                predups += 1;
                logl += logf(predupprob);
            } else {
                dups += 1;
                logl += logf(dupprob);
            }            
        }
    }
    
    //logl += log(poisson(dups, dupprob));
    //logl += log(poisson(predups, predupprob));
    //logl += log(poisson(losses, dupprob)); // assume same rate for loss

    return logl;
}


//=============================================================================
// likelihood functions




void determineFreeBranches(Tree *tree, SpeciesTree *stree, 
                           int *recon, int *events, float generate,
                           int *unfold, float *unfolddist, bool *freebranches)
{
    /*
      find free branches

      A branch is a (partially) free branch if (1) its parent node reconciles to
      the species tree root, (2) it parent node is a duplication, (3) and the
      node it self reconciles not to the species tree root.

      A top branch unfolds, if (1) its parent is the root, (2) its parent
      reconciles to the species tree root, (3) its parent is a duplication, and
      (4) itself does not reconcile to the species tree root. There is atmost one
      unfolding branch per tree.

      If a branch is free, augment its length to min(dist, mean)
    */
    
    int sroot = stree->root->name;
    
    *unfold = -1;
    *unfolddist = 0.0;
    
    for (int i=0; i<tree->nnodes; i++) {
        if (tree->nodes[i]->parent &&
            recon[tree->nodes[i]->parent->name] == sroot &&
            events[tree->nodes[i]->parent->name] == EVENT_DUP &&
            recon[i] != sroot)
        {
            freebranches[i] = true;
        } else {
            freebranches[i] = false;
        }
    }
    
    // find unfolding branch
    if (tree->root->nchildren >= 2 &&
        recon[tree->root->name] == sroot &&
        events[tree->root->name] == EVENT_DUP)
    {
        if (recon[tree->root->children[0]->name] != sroot) {
            *unfold = tree->root->children[0]->name;
            *unfolddist = tree->root->children[1]->dist / generate;
        } else {
            *unfold = tree->root->children[1]->name;
            *unfolddist = tree->root->children[0]->dist / generate;
        }
    }
}



// Generate a random sample of duplication points
void setRandomMidpoints(int root, Tree *tree, SpeciesTree *stree,
                        Node **subnodes, int nsubnodes, 
                        int *recon, int *events, 
                        ReconParams *reconparams,
			float birth, float death)
{
    const float esp = .0001;
    
    // this should not be here
    //reconparams->midpoints[ptree[root]] = 1.0;
    
    for (int i=0; i<nsubnodes; i++) {
        int node = subnodes[i]->name;
        
        if (events[node] == EVENT_DUP) {
            float lastpoint;
            
            if (tree->nodes[node]->parent != NULL && 
		recon[node] == recon[tree->nodes[node]->parent->name])
                // if im the same species branch as my parent 
                // then he is my last midpoint
                lastpoint = reconparams->midpoints[tree->nodes[node]->parent->name];
            else
                // i'm the first on this branch so the last midpoint is zero
                lastpoint = 0.0;
            
            // pick a midpoint uniformly after the last one
            float remain = 1.0 - lastpoint;
            //reconparams->midpoints[node] = lastpoint + esp * remain +
            //                               (1.0-2*esp) * remain * frand();

	    float time = stree->nodes[recon[node]]->dist;
	    reconparams->midpoints[node] = lastpoint + esp * remain +
		sampleBirthWaitTime1(remain * time * (1.0 - esp), 
				     birth, death) / time;

        } else {
            // genes or speciations reconcile exactly to the end of the branch
            // DEL: gene tree roots also reconcile exactly to the end of the branch
            reconparams->midpoints[node] = 1.0;
        }
    }
}


// TODO: remove eventually
// older method, only used by sampler method
BranchParams getBranchParams(int node, Tree *tree, 
			     ReconParams *reconparams)
{
    SpidirParams *params = reconparams->params;
    float totmean = 0.0;
    float totvar = 0.0;
    int snode;
    
    
    float *k = reconparams->midpoints;
    
    int startfrac = reconparams->startfrac[node];
    snode = reconparams->startspecies[node];
    if (startfrac == FRAC_DIFF) {    
        totmean += (k[node] - k[tree->nodes[node]->parent->name]) * 
	    params->sp_alpha[snode];
        totvar  += (k[node] - k[tree->nodes[node]->parent->name]) * 
	    params->sp_beta[snode] * params->sp_beta[snode];
    } else if (startfrac == FRAC_PARENT) {
        float kp = 0;
        
        if (!reconparams->freebranches[node])
            kp = k[tree->nodes[node]->parent->name];
    
        totmean += (1.0 - kp) * params->sp_alpha[snode];
        totvar  += (1.0 - kp) * params->sp_beta[snode] * params->sp_beta[snode];
    }
    // startfrac == FRAC_NONE, do nothing
    
    ExtendArray<int> &path = reconparams->midspecies[node];
    for (int i=0; i<path.size(); i++) {	
	totmean += params->sp_alpha[path[i]];
	float b = params->sp_beta[path[i]];
	totvar += b * b;
    }
    
    int endfrac = reconparams->endfrac[node];
    snode = reconparams->endspecies[node];    
    if (endfrac == FRAC_PARENT) {    
        totmean += (1.0 - k[tree->nodes[node]->parent->name]) * 
	    params->sp_alpha[snode];
        totvar  += (1.0 - k[tree->nodes[node]->parent->name]) * 
	    params->sp_beta[snode] * params->sp_beta[snode];
    } else if (endfrac == FRAC_NODE) {
        totmean += k[node] * params->sp_alpha[snode];
        totvar  += k[node] * params->sp_beta[snode] * params->sp_beta[snode];
    }
    // endfrac == FRAC_NONE, do nothing
    
    return BranchParams(totmean, sqrt(totvar));
}




// Reconcile a branch to the species tree
void reconBranch(int node, Tree *tree, SpeciesTree *stree,
		 int *recon, int *events, 
		 SpidirParams *params,
		 ReconParams *reconparams)
{
    Node** nodes = tree->nodes.get();
    Node** snodes = stree->nodes.get();
    
    
    // set fractional branches
    if (recon[node] == recon[nodes[node]->parent->name]) {
        // start reconciles to a subportion of species branch
        if (events[node] == EVENT_DUP)
            // only case k's are dependent
            reconparams->startfrac[node] = FRAC_DIFF;   // k[node] - k[node.parent]
        else
            reconparams->startfrac[node] = FRAC_PARENT; // 1.0 - k[node.parent]
        
        reconparams->startspecies[node] = recon[node];

        // there is only one frac
        reconparams->endfrac[node] = FRAC_NONE;
        reconparams->endspecies[node] = -1;
    } else {
        if (events[nodes[node]->parent->name] == EVENT_DUP) {
            // start reconciles to last part of species branch
            reconparams->startfrac[node] = FRAC_PARENT; // 1.0 - k[node.parent]
            int snode = recon[nodes[node]->parent->name];
            reconparams->startspecies[node] = snode;
        } else {
            reconparams->startfrac[node] = FRAC_NONE;
            reconparams->startspecies[node] = -1;
        }

        if (events[node] == EVENT_DUP) {
            // end reconciles to first part of species branch
            reconparams->endfrac[node] = FRAC_NODE; // k[node]
            reconparams->endspecies[node] = recon[node];
        } else {
            // end reconcile to at least one whole species branch
            reconparams->endfrac[node] = FRAC_NONE;
            reconparams->endspecies[node] = -1;
        }
    }
    
    // set midparams
    reconparams->midspecies[node].clear();
    if (recon[node] != recon[nodes[node]->parent->name]) {
        // we begin and end on different branches
        int snode;

        // determine most recent species branch which we fully recon to
        if (events[node] == EVENT_DUP)
            snode = snodes[recon[node]]->parent->name;
        else
            snode = recon[node];

        // walk up species spath until starting species branch
        // starting species branch is either fractional or NULL
        
        int parent_snode;
        if (nodes[node]->parent != NULL)
            parent_snode = recon[nodes[node]->parent->name];
        else
            parent_snode = -1;
	
        while (snode != parent_snode && snodes[snode]->parent != NULL) {
	    reconparams->midspecies[node].append(snode);
            snode = snodes[snode]->parent->name;
        }
    } 
    // else {
    //   we begin and end on same branch
    //   there are no midparams
    //   DO NOTHING
    // }


}




// get times vector from midpoints
void getReconTimes(Tree *tree, SpeciesTree *stree, 
		   int node, 
		   ReconParams *reconparams,
		   ExtendArray<float> &times)
{
    float *k = reconparams->midpoints;

    times.clear();   

    
    // start times
    int startfrac = reconparams->startfrac[node];
    if (startfrac == FRAC_DIFF) {    
        times.append((k[node] - k[tree->nodes[node]->parent->name]) *
		     stree->nodes[reconparams->startspecies[node]]->dist);
    } else if (startfrac == FRAC_PARENT) {
        float kp = 0;
        if (!reconparams->freebranches[node])
            kp = k[tree->nodes[node]->parent->name];    
        times.append((1.0 - kp) * 
		     stree->nodes[reconparams->startspecies[node]]->dist);
    } else {
	// startfrac == FRAC_NONE, do nothing
	//times.append(-1.0);
    }

    // mid times
    ExtendArray<int> &path = reconparams->midspecies[node];
    for (int i=0; i<path.size(); i++) {
	times.append(stree->nodes[reconparams->midspecies[node][i]]->dist);
    }
    
    // end time
    int endfrac = reconparams->endfrac[node];
    if (endfrac == FRAC_PARENT) {    
        times.append((1.0 - k[tree->nodes[node]->parent->name]) *
		     stree->nodes[reconparams->endspecies[node]]->dist);
    } else if (endfrac == FRAC_NODE) {
        times.append(k[node] *
		     stree->nodes[reconparams->endspecies[node]]->dist);
    } else {
	// endfrac == FRAC_NONE, do nothing
	//times.append(-1.0);
    }
}




// Calculate branch probability
float branchprob(Tree *tree, SpeciesTree *stree, Node *node,
		 float generate, ReconParams *reconparams,
		 bool approx=true)
{
    const float tol = .001;
    SpidirParams *params = reconparams->params;
    int n = node->name;

    // get times
    ExtendArray<float> times(0, tree->nnodes);
    getReconTimes(tree, stree, n, reconparams, times);
    int nparams = times.size();

    // get gammaSum terms 
    float gs_alpha[nparams];
    float gs_beta[nparams];
    int j = 0;

    if (reconparams->startfrac[n] != FRAC_NONE) {
	int snode = reconparams->startspecies[n];
	gs_alpha[j] = params->sp_alpha[snode];
	gs_beta[j] = (params->sp_beta[snode] / (generate * times[j]));
	j++;
    }

    for (int i=0; i<reconparams->midspecies[n].size(); i++) {
	int snode = reconparams->midspecies[n][i];
	gs_alpha[j] = params->sp_alpha[snode];
	gs_beta[j] = (params->sp_beta[snode] / (generate * times[j]));
	j++;
    }

    if (reconparams->endfrac[n] != FRAC_NONE) {
	int snode = reconparams->endspecies[n];
	gs_alpha[j] = params->sp_alpha[snode];
	gs_beta[j] = (params->sp_beta[snode] / (generate * times[j]));
	j++;
    }

    assert(j == nparams);

    // filter for extreme parameters
    float mean = 0.0;
    for (int i=0; i<nparams; i++) {
	if (isinf(gs_beta[i]) || isnan(gs_beta[i])) {
	    gs_alpha[i] = gs_alpha[--nparams];
	    gs_beta[i] = gs_beta[nparams];
	    i--;
	} else {
	    mean += gs_alpha[i] / gs_beta[i];
	}
    }

    // remove params with effectively zero mean
    const float minfrac = .01;
    float mu = 0.0;
    float var = 0.0;
    for (int i=0; i<nparams; i++) {	
	if (gs_alpha[i] / gs_beta[i] < minfrac * mean) {
	    gs_alpha[i] = gs_alpha[--nparams];
	    gs_beta[i] = gs_beta[nparams];
	    i--;
	} else {
	    mu += gs_alpha[i] / gs_beta[i];
	    var += gs_alpha[i] / gs_beta[i] / gs_beta[i];
	}
    }    
    

    // there is nothing to do
    if (nparams == 0)
	return -INFINITY;
    
    //float dist = node->dist / generate;
    
    float logp;
    if (approx) {
	// approximation
	float a2 = mean*mean/var;
	float b2 = mean/var;
	logp = gammalog(node->dist, a2, b2);
    } else {
	logp = log(gammaSumPdf(node->dist, nparams, gs_alpha, gs_beta, tol));
    }
    
    //float diff = fabs(logp - logp2);

    /*
    if (diff > log(1.3)) {
	printf("\n"
	       "n=%d; l=%f; g=%f\n", n, node->dist, generate);
	printf("t: "); printFloatArray(times, times.size());
	printf("a: "); printFloatArray(gs_alpha, nparams);
	printf("b: "); printFloatArray(gs_beta, nparams);
        
	printf("logp = %f\n", logp);
	printf("logp2 = %f\n", logp2);
	printf("diff = %f\n", diff);
	}*/

    if(isnan(logp))
	logp = -INFINITY;
    return logp;
}



// get roots of speciation subtrees
void getSpecSubtrees(Tree *tree, int *events, ExtendArray<Node*> *rootnodes)
{
    for (int i=0; i<tree->nnodes; i++) {
	if (events[i] == EVENT_SPEC) {
	    for (int j=0; j<2; j++) {
		rootnodes->append(tree->nodes[i]->children[j]);
	    }
	} else if (i == tree->root->name) {
	    rootnodes->append(tree->nodes[i]);
	}
    }
}



void getSubtree(Node *node, int *events, ExtendArray<Node*> *subnodes)
{
    subnodes->append(node);

    // recurse
    if (events[node->name] == EVENT_DUP) {
        for (int i=0; i<node->nchildren; i++) 
            getSubtree(node->children[i], events, subnodes);
    }
}



class BranchPriorCalculator
{
public:
    BranchPriorCalculator(Tree *tree,
			  SpeciesTree *stree,
			  int *recon, int *events, 
			  SpidirParams *params,
			  float brith, float death,
			  int nsamples=1000, bool approx=true) :
        tree(tree),
        stree(stree),
        recon(recon),
        events(events),
        params(params),
	birth(birth),
	death(death),
	nsamples(nsamples),
	approx(approx)
    {
    }
    
    
    ~BranchPriorCalculator()
    {
    }
    


    // subtree prior conditioned on divergence times
    float subtreeprior_cond(Tree *tree, SpeciesTree *stree,
			    int root,
			    int *recon, 
			    float generate,
			    ReconParams *reconparams,
			    ExtendArray<Node*> &subnodes)
    {
	float logp = 0.0;
	int sroot = stree->root->name;
                    
	// loop through all branches in subtree
	for (int j=0; j<subnodes.size(); j++) {
	    int node = subnodes[j]->name;
	
	    if (recon[node] != sroot && node != tree->root->name) {
		logp += branchprob(tree, stree, tree->nodes[node],
				   generate, reconparams);
	    }
	}
            

	return logp;
    }


    // Calculate the likelihood of a subtree
    float subtreeprior(int root, float generate, ReconParams *reconparams)
    {
   
	// set reconparams by traversing subtree
	ExtendArray<Node*> subnodes(0, tree->nnodes);
	getSubtree(tree->nodes[root], events, &subnodes);
    
	// reconcile each branch
	for (int i=0; i<subnodes.size(); i++)
	    if (subnodes[i] != tree->root)
		reconBranch(subnodes[i]->name, tree, stree, 
			    recon, events, params, reconparams);
    
	if (events[root] != EVENT_DUP) {
	    // single branch case
	    return branchprob(tree, stree, tree->nodes[root],
			      generate, reconparams);
	} else {
        
	    // choose number of samples based on number of nodes 
	    // to integrate over
	    //nsamples = int(500*logf(subnodes.size())) + 200;
	    //if (nsamples > 2000) nsamples = 2000;
        	
	    // perform integration by sampling
	    double prob = 0.0;
	    for (int i=0; i<nsamples; i++) {
		double sampleLogl = 0.0;
            
		// propose a setting of midpoints
		// TODO: need to understand why this is here
		if (tree->nodes[root]->parent != NULL)
		    reconparams->midpoints[tree->nodes[root]->parent->name] = 1.0;
		setRandomMidpoints(root, tree, stree,
				   subnodes, subnodes.size(),
				   recon, events, reconparams,
				   birth, death);
            

		sampleLogl = subtreeprior_cond(tree, stree,
					       root,
					       recon, 
					       generate,
					       reconparams,
					       subnodes);
	    
		prob += exp(sampleLogl) / nsamples;
	    }
        
	    return log(prob);
	}
    }


    
    double calc_cond(float generate)
    {
        double logp = 0.0;

        // determine reconciliation parameters
        ReconParams reconparams = ReconParams(tree->nnodes, params);
        determineFreeBranches(tree, stree, recon, events, generate,
                              &reconparams.unfold, 
                              &reconparams.unfolddist, 
                              reconparams.freebranches);

	// determine speciation subtrees
	ExtendArray<Node*> rootnodes(0, tree->nnodes);
	getSpecSubtrees(tree, events, &rootnodes);

	// multiple the probability of each subtree
	for (int i=0; i<rootnodes.size(); i++) {
	    logp += subtreeprior(rootnodes[i]->name, generate, &reconparams);
	}
        
        // gene rate probability
        if (params->gene_alpha > 0 && params->gene_beta > 0)
            logp += gammalog(generate, params->gene_alpha, params->gene_beta);
	
        printLog(LOG_HIGH, "generate: %f %f\n", generate, exp(logp));
        return logp;
    }
    
    inline double calcprob_cond(float generate)
    {
        return exp(calc_cond(generate));
    }


    double calc()
    {
	double maxprob = -INFINITY;
	float argmax_generate = params->gene_alpha / params->gene_beta;
	float est_generate = estimateGeneRate(tree, stree, recon, events, params);
        printLog(LOG_HIGH, "est_generate: %f\n", est_generate);
        
	// TODO: make this equal portions of gamma
        double logp = -INFINITY;       
        float gstart = params->gene_alpha / params->gene_beta * 0.05;
        float gend = params->gene_alpha / params->gene_beta * 3.0;
        float step = (gend - gstart) / 20.0;


	// integrate over gene rates
        for (float g=gstart; g<gend; g+=step) {
            float gi = g + step / 2.0;
            double p = calc_cond(gi);
	    
            logp = logadd(logp, p);
            printLog(LOG_HIGH, "generate_int: %f %f\n", gi, p);
            if (p > maxprob) {
                maxprob = p;
                argmax_generate = gi;
            }
        }
        
        // multiply probabilty by integration step
        logp += log(step);
        
        printLog(LOG_HIGH, "argmax gene rate: %f\n", argmax_generate);
        if (est_generate > 0.0)
            printLog(LOG_HIGH, "branch prior: %f %f %f\n", maxprob, logp,
		     calc_cond(est_generate));

	return logp;
    }
    
    
protected:
    Tree *tree;    
    SpeciesTree *stree;
    int *recon;
    int *events;
    SpidirParams *params;  
    float birth;
    float death;

    int nsamples;
    bool approx;
  
};

  


float branchPrior(Tree *tree,
		  SpeciesTree *stree,
		  int *recon, int *events, SpidirParams *params,
		  float generate,
		  float predupprob, float birth, float death,
		  int nsamples, bool approx)
{
    float logl = 0.0; // log likelihood
        
    
    clock_t startTime = clock();
    
    //srand(time(NULL));
    
    BranchPriorCalculator priorcalc(tree, stree, recon, events, params,
				    birth, death, nsamples, approx);
    
    if (generate > 0) {
        // estimate with given generate
        logl = priorcalc.calc_cond(generate);
    } else {
        logl = priorcalc.calc();
    }
    
    //setLogLevel(LOG_MEDIUM);
    printLog(LOG_MEDIUM, "branch prior time: %f\n", getTimeSince(startTime));
    
    return logl;
}


extern "C" {

// Calculate the likelihood of a tree
float branchPrior(int nnodes, int *ptree, float *dists,
		  int nsnodes, int *pstree, float *sdists,
		  int *recon, int *events,
		  float *sp_alpha, float *sp_beta, float generate,
		  float predupprob, float birth, float death,
		  float gene_alpha, float gene_beta,
		  int nsamples, bool approx)
{
    // create tree objects
    Tree tree(nnodes);
    ptree2tree(nnodes, ptree, &tree);
    tree.setDists(dists);
    
    SpeciesTree stree(nsnodes);
    ptree2tree(nsnodes, pstree, &stree);
    stree.setDepths();
    stree.setDists(sdists);

    srand(time(NULL));
    
    SpidirParams params(nsnodes, NULL, sp_alpha, sp_beta, 
			gene_alpha, gene_beta);
    
    return branchPrior(&tree, &stree,
		       recon, events, &params, 
		       generate, 
		       predupprob, birth, death,
		       nsamples, approx);
}

} // extern "C"









//=============================================================================
// Gene rate estimation


// functor of gene rate posterior derivative
class GeneRateDerivative 
{
public:
    GeneRateDerivative(Tree *tree, SpeciesTree *stree,
                       int *recon, int *events, SpidirParams *params) :
        lkcalc(tree, stree, recon, events, params, 0.0001, 0.0002)
    {
    }
    
    float operator()(float generate)
    {
        const float step = .05;
        return lkcalc.calc_cond(generate + step) - lkcalc.calc_cond(generate);
    }
    
    BranchPriorCalculator lkcalc;
};


// Ignores sequence data, assumes given branch lengths are perfectly known
float maxPosteriorGeneRate(Tree *tree, SpeciesTree *stree,
                           int *recon, int *events, SpidirParams *params)
{
    // get initial estimate of gene rate and a bound to search around
    float est_generate = estimateGeneRate(tree, stree, recon, events, params);
    float maxg = est_generate * 1.5; //params->alpha / params->beta * 2.0;
    float ming = est_generate / 1.5;
    
    // find actual max posterior of gene rate
    GeneRateDerivative df(tree, stree, recon, events, params);
    return bisectRoot(df, ming, maxg, (maxg - ming) / 1000.0);
}


float maxPosteriorGeneRate(int nnodes, int *ptree, float *dists,
                           int nsnodes, int *pstree, 
                           int *recon, int *events,
                           float *mu, float *sigma,
                           float alpha, float beta)
{
    // create tree objects
    Tree tree(nnodes);
    ptree2tree(nnodes, ptree, &tree);
    tree.setDists(dists);
    
    SpeciesTree stree(nsnodes);
    ptree2tree(nsnodes, pstree, &stree);
    stree.setDepths();    

    SpidirParams params = SpidirParams(nsnodes, NULL, mu, sigma, alpha, beta);
    
    return maxPosteriorGeneRate(&tree, &stree, recon, events, &params);
}


void samplePosteriorGeneRate(Tree *tree, 
                             int nseqs, char **seqs, 
                             const float *bgfreq, float ratio,
                             SpeciesTree *stree, 
                             int *gene2species,
                             SpidirParams *params,
                             int nsamples,
                             geneRateCallback callback,
                             void *userdata)
{
    // use parsimonious reconciliation as default
    ExtendArray<int> recon(tree->nnodes);
    ExtendArray<int> events(tree->nnodes);
    reconcile(tree, stree, gene2species, recon);
    labelEvents(tree, recon, events);

    // check events
    for (int i=0; i<tree->nnodes; i++) {
	assert(events[i] <= 2 && events[i] >= 0);
    }

    samplePosteriorGeneRate(tree,
                            nseqs, seqs, 
                            bgfreq, ratio,
                            stree,
                            recon, events, params,
                            nsamples,
                            callback,
                            userdata);
}


// Uses MCMC to sample from P(B,G|T,D)
void samplePosteriorGeneRate(Tree *tree,
                             int nseqs, char **seqs, 
                             const float *bgfreq, float ratio,
                             SpeciesTree *stree,
                             int *recon, int *events, SpidirParams *params,
                             int nsamples,
                             geneRateCallback callback,
                             void *userdata)
{
    // state variables B, G
    ExtendArray<float> dists(tree->nnodes);
    float next_generate = 0, generate = 0;
    float logl = -INFINITY;
    float logl2 = 999;
    float next_logl, next_logl2;
    float alpha;

    const float generate_step = .2;
    const float min_generate = .0001;

    // initialize first state
    generate = gammavariate(params->gene_alpha, params->gene_beta);
    generateBranchLengths(tree, stree,
                          recon, events,
                          params, generate);

    
    // perform MCMC
    for (int i=0; i<nsamples; i++) {
        // generate a new state 
	//printf("sample %d\n", i);
     
	for (int j=0; j<tree->nnodes; j++) {
	    assert(events[j] <= 2 && events[j] >= 0);

	    // NAN distances
	    if (isnan(tree->nodes[j]->dist)) {
		tree->nodes[j]->dist = min_generate;
	    }	    
	}
 
        if (frand() < .5) {
	    //printf("sample G\n");
            printLog(LOG_HIGH, "sample G: ");

            // sample G_2
            next_generate = frand(max(generate - generate_step, min_generate),
                                  generate + generate_step);
            //next_generate = gammavariate(params->alpha, params->beta);

            // if P(B_1|T,G_1) not exist, make it
            if (logl2 > 0) {                
                logl2 = branchPrior(tree, stree, recon, events, params,
				    generate,
				    1, 1, 1);
            }
	    
            // set new branch lengths B_2
            for (int j=0; j<tree->nnodes; j++) {
                tree->nodes[j]->dist *= next_generate / generate;

		// sometimes when generate change is drastic we need to catch
		// NAN distances
		if (isnan(tree->nodes[j]->dist)) {
		    tree->nodes[j]->dist = min_generate;
		}
	    }

            
            // calculate P(D|B_2,T)
            next_logl = calcSeqProbHky(tree, nseqs, seqs, bgfreq, ratio);
	    
            // calculate P(B_2|T,G_2)
            next_logl2 = branchPrior(tree, stree, recon, events, params,
				     next_generate,
				     1, 1, 1);

            // q(G_1|G_2) = 1 /(next_generate + generate_step - 
            //                  max(next_generate - generate_step, 
            //                      min_generate))
            // q(G_2|G_1) = 1 /(generate + generate_step - 
            //                  max(generate - generate_step, 
            //                      min_generate))
            // qratio = (generate + generate_step - 
            //                  max(generate - generate_step, 
            //                      min_generate)) /
            //          (next_generate + generate_step - 
            //                  max(next_generate - generate_step, 
            //                      min_generate))


            float logl3 = gammalog(next_generate, 
                                   params->gene_alpha, params->gene_beta) - 
                          gammalog(generate, 
                                   params->gene_alpha, params->gene_beta) +
                          log((generate + generate_step - 
                               max(generate - generate_step, 
                                   min_generate)) /
                              (next_generate + generate_step - 
                               max(next_generate - generate_step, 
                                   min_generate)));

            alpha = exp(next_logl + next_logl2 - logl - logl2 + logl3);

        } else {
	    //printf("sample B\n");
            printLog(LOG_HIGH, "sample B: ");

            // keep same gene rate G_2 = G_1
            next_generate = generate;
            
            // sample B_2
            generateBranchLengths(tree, stree,
                                  recon, events,
                                  params, next_generate, -2, -2);
	    
	    for (int j=0; j<tree->nnodes; j++) {
		// NAN distances
		if (isnan(tree->nodes[j]->dist)) {
		    tree->nodes[j]->dist = min_generate;
		}	    
	    }

            // calculate P(D|B_2,T)
            next_logl = calcSeqProbHky(tree, nseqs, seqs, bgfreq, ratio);
            next_logl2 = 999;
            
            alpha = exp(next_logl - logl);
        }
        

        if (frand() <= alpha) {
            // accept: logl, G, B
            printLog(LOG_HIGH, "accept\n");
            logl = next_logl;
            logl2 = next_logl2;
            generate = next_generate;
            tree->getDists(dists);
        } else {
            // reject
            printLog(LOG_HIGH, "reject\n");
            
            // restore previous B
            tree->setDists(dists);
        }
        
        // return sample
        callback(generate, tree, userdata);
    }
}




// Uses MCMC to sample from P(B,G|T,D)
void samplePosteriorGeneRate_old(Tree *tree,
                             int nseqs, char **seqs, 
                             const float *bgfreq, float ratio,
                             SpeciesTree *stree,
                             int *recon, int *events, SpidirParams *params,
                             int nsamples,
                             geneRateCallback callback,
                             void *userdata)
{
    // state variables B, G
    ExtendArray<float> dists(tree->nnodes);
    float next_generate = 0, generate = 0;
    float logl = -INFINITY;
    float next_logl;
    
    for (int i=0; i<nsamples; i++) {
        // generate a new state 

        // sample G
        next_generate = gammavariate(params->gene_alpha, params->gene_beta);

        // sample B
        generateBranchLengths(tree, stree,
                              recon, events,
                              params, next_generate);
        
        // calculate P(D|B,T)
        next_logl = calcSeqProbHky(tree, nseqs, seqs, bgfreq, ratio);

        //printf(">> %f %f\n", next_logl, logl);

        float alpha = exp(next_logl - logl);
        if (frand() <= alpha) {
            // accept: logl, G, B
            printLog(LOG_HIGH, "accept: %f, %f\n", next_logl, logl);
            logl = next_logl;
            generate = next_generate;
            tree->getDists(dists);
        } else {
            // reject
            printLog(LOG_HIGH, "reject: %f, %f\n", next_logl, logl);
            
            // restore previous B
            tree->setDists(dists);
        }
        
        // return sample
        callback(generate, tree, userdata);
    }
}



/*
// get nodes in preorder (starting with given node)
void getSubtree(int **ftree, int node, int *events, ExtendArray<int> *subnodes)
{
    subnodes->append(node);

    // recurse
    if (events[node] == EVENT_DUP) {
        if (ftree[node][0] != -1)
            getSubtree(ftree, ftree[node][0], events, subnodes);
        if (ftree[node][1] != -1)
            getSubtree(ftree, ftree[node][1], events, subnodes);
    }
}


// Calculate branch likelihood
float branchlk(float dist, int node, int *ptree, ReconParams *reconparams)
{
    BranchParams bparam = getBranchParams(node, ptree, reconparams);
    float totmean = bparam.alpha;
    float totsigma = bparam.beta;
    
    // handle partially-free branches and unfold
    if (reconparams->unfold == node)
        dist += reconparams->unfolddist;
    
    // augment a branch if it is partially free
    if (reconparams->freebranches[node]) {
        if (dist > totmean)
            dist = totmean;
    }
    
    // TODO: should skip this branch in the first place
    if (totsigma == 0.0)
        return 0.0;
    
    float logl = normallog(dist, totmean, totsigma);
    
    if (isnan(logl)) {
	printf("dist=%f, totmean=%f, totsigma=%f\n", dist, totmean, totsigma);
	assert(0);
    }
    return logl;
}



BranchParams getBranchParams(int node, int *ptree, ReconParams *reconparams)
{
    float totmean = 0.0;
    float totvar = 0.0;
    BranchParams bparam;
    
    
    float *k = reconparams->midpoints;
    
    int startfrac = reconparams->startfrac[node];
    bparam = reconparams->startparams[node];
    if (startfrac == FRAC_DIFF) {    
        totmean += (k[node] - k[ptree[node]]) * bparam.alpha;
        totvar  += (k[node] - k[ptree[node]]) * bparam.beta * bparam.beta;
    } else if (startfrac == FRAC_PARENT) {
        float kp = 0;
        
        if (!reconparams->freebranches[node])
            kp = k[ptree[node]];
    
        totmean += (1.0 - kp) * bparam.alpha;
        totvar  += (1.0 - kp) * bparam.beta * bparam.beta;
    }
    // startfrac == FRAC_NONE, do nothing
    
    bparam = reconparams->midparams[node];
    if (!bparam.isNull()) {
        if (bparam.alpha < 0) {
            fprintf(stderr, "bparam %f\n", bparam.alpha);
            fprintf(stderr, "node %d\n", node);
            assert(0);
        }
        totmean += bparam.alpha;
        totvar  += bparam.beta * bparam.beta;
    }
    
    int endfrac = reconparams->endfrac[node];
    bparam = reconparams->endparams[node];    
    if (endfrac == FRAC_PARENT) {    
        totmean += (1.0 - k[ptree[node]]) * bparam.alpha;
        totvar  += (1.0 - k[ptree[node]]) * bparam.beta * bparam.beta;
    } else if (endfrac == FRAC_NODE) {
        totmean += k[node] * bparam.alpha;
        totvar  += k[node] * bparam.beta * bparam.beta;
    }
    // endfrac == FRAC_NONE, do nothing
    
    return BranchParams(totmean, sqrt(totvar));
}




// Calculate branch likelihood
float branchlk(float dist, int node, Tree *tree, 
	       ReconParams *reconparams)
{
    BranchParams bparam = getBranchParams(node, tree, reconparams);
    float totmean = bparam.alpha;
    float totsigma = bparam.beta;
    
    // handle partially-free branches and unfold
    if (reconparams->unfold == node)
        dist += reconparams->unfolddist;
    
    // augment a branch if it is partially free
    if (reconparams->freebranches[node]) {
        if (dist > totmean)
            dist = totmean;
    }
    
    // TODO: should skip this branch in the first place
    if (totsigma == 0.0)
        return 0.0;
    
    float logl = normallog(dist, totmean, totsigma);
    
    if (isnan(logl)) {
	printf("dist=%f, totmean=%f, totsigma=%f\n", dist, totmean, totsigma);
	assert(0);
    }
    return logl;
}



*/




} // namespace spidir
