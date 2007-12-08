/*
 * click-align.cc -- alignment enforcer for Click configurations
 * Eddie Kohler
 *
 * Copyright (c) 1999-2000 Massachusetts Institute of Technology
 * Copyright (c) 2007 Regents of the University of California
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>
#include <click/pathvars.h>

#include <click/error.hh>
#include <click/confparse.hh>
#include <click/straccum.hh>
#include <click/driver.hh>
#include "lexert.hh"
#include "routert.hh"
#include "alignment.hh"
#include "alignclass.hh"
#include "elementmap.hh"
#include "toolutils.hh"
#include <click/clp.h>
#include <stdio.h>
#include <ctype.h>
#include <algorithm>

static ElementMap element_map;

struct RouterAlign {

    RouterT *_router;
    Vector<int> _icount;
    Vector<int> _ocount;
    Vector<int> _ioffset;
    Vector<int> _ooffset;
    Vector<Alignment> _ialign;
    Vector<Alignment> _oalign;
    Vector<Aligner *> _aligners;

    RouterAlign(RouterT *, ErrorHandler *);

  int iindex_eindex(int) const;
  int iindex_port(int) const;
  int oindex_eindex(int) const;
  int oindex_port(int) const;
  
  bool have_input();
  void have_output();
  
  void want_input();
  bool want_output();

  void adjust();
  
  void print(FILE *);
  
};

RouterAlign::RouterAlign(RouterT *r, ErrorHandler *errh)
    : _router(r)
{
  int ne = r->nelements();
  int id = 0, od = 0;
  for (int i = 0; i < ne; i++) {
    _ioffset.push_back(id);
    _ooffset.push_back(od);
    ElementT *e = r->element(i);
    _icount.push_back(e->ninputs());
    _ocount.push_back(e->noutputs());
    id += e->ninputs();
    od += e->noutputs();
  }
  _ioffset.push_back(id);
  _ooffset.push_back(od);
  // set alignments
  _ialign.assign(id, Alignment());
  _oalign.assign(od, Alignment());
  // find aligners
  _aligners.assign(_router->nelements(), default_aligner());
  for (RouterT::iterator x = _router->begin_elements(); x; x++) {
    AlignClass *eclass = (AlignClass *)x->type()->cast("AlignClass");
    if (eclass)
      _aligners[x->eindex()] = eclass->create_aligner(x, _router, errh);
  }
}

int
RouterAlign::iindex_eindex(int ii) const
{
  int ne = _icount.size();
  for (int i = 0; i < ne; i++)
    if (ii < _ioffset[i+1])
      return i;
  return -1;
}

int
RouterAlign::iindex_port(int ii) const
{
  int ne = _icount.size();
  for (int i = 0; i < ne; i++)
    if (ii < _ioffset[i+1])
      return ii - _ioffset[i];
  return -1;
}

int
RouterAlign::oindex_eindex(int oi) const
{
  int ne = _icount.size();
  for (int i = 0; i < ne; i++)
    if (oi < _ooffset[i+1])
      return i;
  return -1;
}

int
RouterAlign::oindex_port(int oi) const
{
  int ne = _icount.size();
  for (int i = 0; i < ne; i++)
    if (oi < _ooffset[i+1])
      return oi - _ooffset[i];
  return -1;
}

void
RouterAlign::have_output()
{
  int ne = _icount.size();
  for (int i = 0; i < ne; i++) {
      Traits t = element_map.traits(_router->etype_name(i));
      _aligners[i]->have_flow(_ialign.begin() + _ioffset[i], _icount[i],
			      _oalign.begin() + _ooffset[i], _ocount[i],
			      t.flow_code);
  }
}

bool
RouterAlign::have_input()
{
  const Vector<ConnectionT> &conn = _router->connections();
  int nh = conn.size();
  int nialign = _ialign.size();

  Vector<Alignment> new_ialign(nialign, Alignment());
  for (int i = 0; i < nh; i++)
    if (conn[i].live()) {
      int ioff = _ioffset[conn[i].to_eindex()] + conn[i].to_port();
      int ooff = _ooffset[conn[i].from_eindex()] + conn[i].from_port();
      new_ialign[ioff] |= _oalign[ooff];
    }

  // see if anything happened
  bool changed = false;
  for (int i = 0; i < nialign && !changed; i++)
    if (new_ialign[i] != _ialign[i])
      changed = true;
  _ialign.swap(new_ialign);
  return changed;
}

void
RouterAlign::want_input()
{
  int ne = _icount.size();
  for (int i = 0; i < ne; i++) {
      Traits t = element_map.traits(_router->etype_name(i));
      _aligners[i]->want_flow(_ialign.begin() + _ioffset[i], _icount[i],
			      _oalign.begin() + _ooffset[i], _ocount[i],
			      t.flow_code);
  }
}

bool
RouterAlign::want_output()
{
  const Vector<ConnectionT> &conn = _router->connections();
  int nh = conn.size();
  int noalign = _oalign.size();

  Vector<Alignment> new_oalign(noalign, Alignment());
  for (int i = 0; i < nh; i++)
    if (conn[i].live()) {
      int ioff = _ioffset[conn[i].to_eindex()] + conn[i].to_port();
      int ooff = _ooffset[conn[i].from_eindex()] + conn[i].from_port();
      new_oalign[ooff] &= _ialign[ioff];
    }
  /* for (int i = 0; i < noalign; i++)
    if (new_oalign[i].bad())
      new_oalign[i] = Alignment(); */

  // see if anything happened
  bool changed = false;
  for (int i = 0; i < noalign && !changed; i++)
    if (new_oalign[i] != _oalign[i]) {
      /* fprintf(stderr, "%s[%d] %s <- %s\n", _router->ename(oindex_eindex(i)).c_str(), oindex_port(i), new_oalign[i].s().c_str(), _oalign[i].s().c_str()); */
      changed = true;
    }
  _oalign.swap(new_oalign);
  return changed;
}

void
RouterAlign::adjust()
{
  int ne = _icount.size();
  for (int i = 0; i < ne; i++)
      _aligners[i]->adjust_flow(_ialign.begin() + _ioffset[i], _icount[i],
				_oalign.begin() + _ooffset[i], _ocount[i]);
}

void
RouterAlign::print(FILE *f)
{
  for (RouterT::iterator x = _router->begin_elements(); x; x++) {
    int i = x->eindex();
    fprintf(f, "%s :", x->name_c_str());
    for (int j = 0; j < _icount[i]; j++) {
      const Alignment &a = _ialign[ _ioffset[i] + j ];
      fprintf(f, " %d/%d", a.chunk(), a.offset());
    }
    fprintf(f, " -");
    for (int j = 0; j < _ocount[i]; j++) {
      const Alignment &a = _oalign[ _ooffset[i] + j ];
      fprintf(f, " %d/%d", a.chunk(), a.offset());
    }
    fprintf(f, "\n");
  }
  fprintf(f, "\n");
}


static ElementClassT *class_factory(const String &name)
{
    if (name == "Align")
	return new AlignAlignClass;
    if (name == "Strip")
	return new StripAlignClass;
    if (name == "CheckIPHeader" || name == "CheckIPHeader2")
	return new CheckIPHeaderAlignClass(name, 1);
    if (name == "MarkIPHeader")
	return new CheckIPHeaderAlignClass(name, 0);
    if (name == "Classifier")
	return new AlignClass(name, new ClassifierAligner);
    if (name == "EtherEncap")
	return new AlignClass(name, new ShifterAligner(-14));
    if (name == "FromDevice" || name == "PollDevice" || name == "FromHost"
	|| name == "SR2SetChecksum" || name == "SR2CheckHeader"
	|| name == "SetSRChecksum" || name == "CheckSRHeader")
	return new AlignClass(name, new GeneratorAligner(Alignment(4, 2)));
    if (name == "InfiniteSource" || name == "RatedSource"
	|| name == "ICMPError")
	return new AlignClass(name, new GeneratorAligner(Alignment(4, 0)));
    if (name == "ToHost")
	return new AlignClass(name, new WantAligner(Alignment(4, 2)));
    if (name == "IPEncap" || name == "UDPIPEncap" || name == "ICMPPingEncap"
	|| name == "RandomUDPIPEncap" || name == "RoundRobinUDPIPEncap"
	|| name == "RoundRobinTCPIPEncap")
	return new AlignClass(name, new WantAligner(Alignment(4, 0)));
    if (name == "ARPResponder" || name == "ARPQuerier")
	return new AlignClass(name, new WantAligner(Alignment(2, 0)));
    if (name == "IPInputCombo")
	return new AlignClass(name, new CombinedAligner(new ShifterAligner(14),
							new WantAligner(Alignment(4, 2))));
    if (name == "GridEncap")
	return new AlignClass(name, new CombinedAligner(new ShifterAligner(98),
							new WantAligner(Alignment(4, 0))));
    if (name == "Idle" || name == "Discard")
	return new AlignClass(name, new NullAligner);
    return new AlignClass(name, default_aligner());
}


#define HELP_OPT		300
#define VERSION_OPT		301
#define ROUTER_OPT		303
#define EXPRESSION_OPT		304
#define OUTPUT_OPT		305

#define FIRST_DRIVER_OPT	1000
#define USERLEVEL_OPT		(1000 + Driver::USERLEVEL)
#define LINUXMODULE_OPT		(1000 + Driver::LINUXMODULE)
#define BSDMODULE_OPT		(1000 + Driver::BSDMODULE)

static Clp_Option options[] = {
  { "bsdmodule", 'b', BSDMODULE_OPT, 0, 0 },
  { "expression", 'e', EXPRESSION_OPT, Clp_ArgString, 0 },
  { "file", 'f', ROUTER_OPT, Clp_ArgString, 0 },
  { "help", 0, HELP_OPT, 0, 0 },
  { "linuxmodule", 'l', LINUXMODULE_OPT, 0, 0 },
  { "output", 'o', OUTPUT_OPT, Clp_ArgString, 0 },
  { "userlevel", 'u', USERLEVEL_OPT, 0, 0 },
  { "version", 'v', VERSION_OPT, 0, 0 },
};

static const char *program_name;
static int specified_driver = -1;

void
short_usage()
{
  fprintf(stderr, "Usage: %s [OPTION]... [ROUTERFILE]\n\
Try '%s --help' for more information.\n",
	  program_name, program_name);
}

void
usage()
{
  printf("\
'Click-align' adds any required 'Align' elements to a Click router\n\
configuration. The resulting router will work on machines that don't allow\n\
unaligned accesses. Its configuration is written to the standard output.\n\
\n\
Usage: %s [OPTION]... [ROUTERFILE]\n\
\n\
Options:\n\
  -f, --file FILE               Read router configuration from FILE.\n\
  -e, --expression EXPR         Use EXPR as router configuration.\n\
  -o, --output FILE             Write output to FILE.\n\
      --help                    Print this message and exit.\n\
  -v, --version                 Print version number and exit.\n\
\n\
Report bugs to <click@pdos.lcs.mit.edu>.\n", program_name);
}


inline String
aligner_name(int anonymizer)
{
  return String("Align@click_align@") + String(anonymizer);
}

int
main(int argc, char **argv)
{
  click_static_initialize();
  CLICK_DEFAULT_PROVIDES;
  ErrorHandler *errh = ErrorHandler::default_handler();
  PrefixErrorHandler p_errh(errh, "click-align: ");

  // read command line arguments
  Clp_Parser *clp =
    Clp_NewParser(argc, argv, sizeof(options) / sizeof(options[0]), options);
  Clp_SetOptionChar(clp, '+', Clp_ShortNegated);
  program_name = Clp_ProgramName(clp);

  const char *router_file = 0;
  bool file_is_expr = false;
  const char *output_file = 0;
  
  while (1) {
    int opt = Clp_Next(clp);
    switch (opt) {

     case HELP_OPT:
      usage();
      exit(0);
      break;
      
     case VERSION_OPT:
      printf("click-align (Click) %s\n", CLICK_VERSION);
      printf("Copyright (C) 1999-2000 Massachusetts Institute of Technology\n\
Copyright (C) 2007 Regents of the University of California\n\
This is free software; see the source for copying conditions.\n\
There is NO warranty, not even for merchantability or fitness for a\n\
particular purpose.\n");
      exit(0);
      break;
      
     case ROUTER_OPT:
     case EXPRESSION_OPT:
     router_file:
      if (router_file) {
	errh->error("router configuration specified twice");
	goto bad_option;
      }
      router_file = clp->arg;
      file_is_expr = (opt == EXPRESSION_OPT);
      break;

      case USERLEVEL_OPT:
      case LINUXMODULE_OPT:
      case BSDMODULE_OPT:
	if (specified_driver >= 0) {
	    errh->error("driver specified twice");
	    goto bad_option;
	}
	specified_driver = opt - FIRST_DRIVER_OPT;
	break;

     case Clp_NotOption:
      if (!click_maybe_define(clp->arg, errh))
	  goto router_file;
      break;

     case OUTPUT_OPT:
      if (output_file) {
	errh->error("output file specified twice");
	goto bad_option;
      }
      output_file = clp->arg;
      break;
      
     bad_option:
     case Clp_BadOption:
      short_usage();
      exit(1);
      break;
      
     case Clp_Done:
      goto done;
      
    }
  }
  
  done:
    // read router
    ElementClassT::set_base_type_factory(class_factory);
    RouterT *router = read_router(router_file, file_is_expr, errh);
    if (router)
	router->flatten(errh);
    if (!router || errh->nerrors() > 0)
	exit(1);

    // get element map and processing
    element_map.parse_all_files(router, CLICK_DATADIR, &p_errh);

    // we know that Classifier requires alignment.  If the element map does
    // not, complain loudly.
    if (!element_map.has_traits("Classifier")) {
	p_errh.warning("elementmap has no information for Classifier, muddling along");
	p_errh.message("(This is usually because of a missing 'elementmap.xml'.\nYou may get errors when running this configuration.)");
	const char *classes[] = { "Classifier", "IPClassifier", "IPFilter", "CheckIPHeader", "CheckIPHeader2", "UDPIPEncap", "IPInputCombo", 0 };
	for (const char **sp = classes; *sp; ++sp) {
	    ElementTraits t = element_map.traits(*sp);
	    t.flags += "A";
	    element_map.add(t);
	}
    }

    // decide on a driver
    if (specified_driver >= 0) {
	if (!element_map.driver_compatible(router, specified_driver))
	    errh->warning("configuration not compatible with %s driver", Driver::name(specified_driver));
	element_map.set_driver_mask(1 << specified_driver);
    } else {
	int driver_mask = 0;
	for (int d = 0; d < Driver::COUNT; d++)
	    if (element_map.driver_compatible(router, d))
		driver_mask |= 1 << d;
	if (driver_mask == 0)
	    errh->warning("configuration not compatible with any driver");
	else
	    element_map.set_driver_mask(driver_mask);
    }

    // correct classes for router
    //correct_classes(driver_mask);

    ElementClassT *align_class = ElementClassT::base_type("Align");
    assert(align_class);

    int original_nelements = router->nelements();
    int anonymizer = original_nelements + 1;
    while (router->eindex(aligner_name(anonymizer)) >= 0)
	anonymizer++;

    /*
     * Add Align elements for required alignments
     */
    int num_aligns_added = 0;
    {
	// calculate current alignment
	RouterAlign ral(router, errh);
	do {
	    ral.have_output();
	} while (ral.have_input());
    
	// calculate desired alignment
	RouterAlign want_ral(router, errh);
	do {
	    want_ral.want_input();
	} while (want_ral.want_output());

	// add required aligns
	for (int i = 0; i < original_nelements; i++)
	    for (int j = 0; j < ral._icount[i]; j++) {
		Alignment have = ral._ialign[ ral._ioffset[i] + j ];
		Alignment want = want_ral._ialign[ ral._ioffset[i] + j ];
		if (have <= want || want.bad())
		    /* do nothing */;
		else {
		    ElementT *e = router->get_element
			(aligner_name(anonymizer), align_class,
			 String(want.chunk()) + ", " + String(want.offset()),
			 LandmarkT("<click-align>"));
		    router->insert_before(e, PortT(router->element(i), j));
		    anonymizer++;
		    num_aligns_added++;
		}
	    }
    }

    /*
     * remove duplicate Aligns (Align_1 -> Align_2)
     */
    {
	const Vector<ConnectionT> &conn = router->connections();
	int nhook = conn.size();
	for (int i = 0; i < nhook; i++)
	    if (conn[i].live() && conn[i].to_element()->type() == align_class
		&& conn[i].from_element()->type() == align_class) {
		// skip over hf[i]
		Vector<PortT> above, below;
		router->find_connections_to(conn[i].from(), above);
		router->find_connections_from(conn[i].from(), below);
		if (below.size() == 1) {
		    for (int j = 0; j < nhook; j++)
			if (conn[j].to() == conn[i].from())
			    router->change_connection_to(j, conn[i].to());
		} else if (above.size() == 1) {
		    for (int j = 0; j < nhook; j++)
			if (conn[j].to() == conn[i].to())
			    router->change_connection_from(j, above[0]);
		}
	    }
    }

    /*
     * Add Aligns required for adjustment alignments
     *
     * Classifier is an example: it can cope with any alignment that is
     * consistent and has a chunk >= 4. Rather than require a specific
     * alignment above, we handle Classifier here; required alignments may
     * have generated an alignment we can deal with.
     */
    {
	// calculate current alignment
	RouterAlign ral(router, errh);
	do {
	    ral.have_output();
	} while (ral.have_input());
    
	// calculate adjusted alignment
	RouterAlign want_ral(ral);
	want_ral.adjust();

	// add required aligns
	for (int i = 0; i < original_nelements; i++)
	    for (int j = 0; j < ral._icount[i]; j++) {
		Alignment have = ral._ialign[ ral._ioffset[i] + j ];
		Alignment want = want_ral._ialign[ ral._ioffset[i] + j ];
		if (have <= want || want.bad())
		    /* do nothing */;
		else {
		    ElementT *e = router->get_element
			(aligner_name(anonymizer), align_class,
			 String(want.chunk()) + ", " + String(want.offset()),
			 LandmarkT("<click-align>"));
		    router->insert_before(e, PortT(router->element(i), j));
		    anonymizer++;
		    num_aligns_added++;
		}
	    }
    }

    while (1) {
	// calculate current alignment
	RouterAlign ral(router, errh);
	do {
	    ral.have_output();
	} while (ral.have_input());

	bool changed = false;
    
	// skip redundant Aligns
	const Vector<ConnectionT> &conn = router->connections();
	int nhook = conn.size();
	for (int i = 0; i < nhook; i++)
	    if (conn[i].live() && conn[i].to_element()->type() == align_class) {
		Alignment have = ral._oalign[ ral._ooffset[conn[i].from_eindex()] + conn[i].from_port() ];
		Alignment want = ral._oalign[ ral._ooffset[conn[i].to_eindex()] ];
		if (have <= want) {
		    changed = true;
		    Vector<PortT> align_dest;
		    router->find_connections_from(conn[i].to(), align_dest);
		    for (int j = 0; j < align_dest.size(); j++)
			router->add_connection(conn[i].from(), align_dest[j]);
		    router->kill_connection(i);
		}
	    }

	if (!changed)
	    break;
    
	router->remove_duplicate_connections();
    }

    // remove unused Aligns (they have no input) and old AlignmentInfos
    ElementClassT *aligninfo_class = ElementClassT::base_type("AlignmentInfo");
    {
	for (RouterT::iterator x = router->begin_elements(); x; x++)
	    if (x->type() == align_class
		&& (x->ninputs() == 0 || x->noutputs() == 0)) {
		x->kill();
		num_aligns_added--;
	    } else if (x->type() == aligninfo_class)
		x->kill();
	router->remove_dead_elements();
    }
  
    // make the AlignmentInfo element
    {
	RouterAlign ral(router, errh);
	do {
	    ral.have_output();
	} while (ral.have_input());

	// collect alignment information, delete old AlignmentInfos
	StringAccum sa;
	for (RouterT::iterator x = router->begin_elements(); x; x++) {
	    ElementTraits t = element_map.traits(x->type_name());
	    if (x->ninputs() && t.flag_value('A') > 0) {
		if (sa.length())
		    sa << ",\n  ";
		sa << x->name();
		int i = x->eindex();
		for (int j = 0; j < ral._icount[i]; j++) {
		    const Alignment &a = ral._ialign[ ral._ioffset[i] + j ];
		    sa << "  " << a.chunk() << " " << a.offset();
		}
	    }
	}

	if (sa.length())
	    router->get_element(String("AlignmentInfo@click_align@")
				+ String(router->nelements() + 1),
				aligninfo_class, sa.take_string(),
				LandmarkT("<click-align>"));
    }

  // warn if added aligns
  if (num_aligns_added > 0)
    errh->warning((num_aligns_added > 1 ? "added %d Align elements" : "added %d Align element"), num_aligns_added);
  
  // write result
  if (write_router_file(router, output_file, errh) < 0)
    exit(1);
  return 0;
}

// generate Vector template instance
#include <click/vector.cc>
template class Vector<Alignment>;
