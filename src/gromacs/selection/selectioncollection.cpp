/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2010,2011,2012, by the GROMACS development team, led by
 * David van der Spoel, Berk Hess, Erik Lindahl, and including many
 * others, as listed in the AUTHORS file in the top-level source
 * directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */
/*! \internal \file
 * \brief
 * Implements gmx::SelectionCollection.
 *
 * \author Teemu Murtola <teemu.murtola@gmail.com>
 * \ingroup module_selection
 */
#include "selectioncollection.h"

#include <cstdio>

#include <boost/shared_ptr.hpp>

#include "gromacs/legacyheaders/oenv.h"
#include "gromacs/legacyheaders/smalloc.h"
#include "gromacs/legacyheaders/xvgr.h"

#include "gromacs/options/basicoptions.h"
#include "gromacs/options/options.h"
#include "gromacs/selection/selection.h"
#include "gromacs/utility/exceptions.h"
#include "gromacs/utility/file.h"
#include "gromacs/utility/gmxassert.h"
#include "gromacs/utility/messagestringcollector.h"
#include "gromacs/utility/stringutil.h"

#include "compiler.h"
#include "mempool.h"
#include "parser.h"
#include "poscalc.h"
#include "scanner.h"
#include "selectioncollection-impl.h"
#include "selelem.h"
#include "selhelp.h"
#include "selmethod.h"
#include "symrec.h"

namespace gmx
{

/********************************************************************
 * SelectionCollection::Impl
 */

SelectionCollection::Impl::Impl()
    : debugLevel_(0), bExternalGroupsSet_(false), grps_(NULL)
{
    sc_.nvars     = 0;
    sc_.varstrs   = NULL;
    sc_.top       = NULL;
    gmx_ana_index_clear(&sc_.gall);
    sc_.mempool   = NULL;
    sc_.symtab.reset(new SelectionParserSymbolTable);
    gmx_ana_selmethod_register_defaults(sc_.symtab.get());
}


SelectionCollection::Impl::~Impl()
{
    clearSymbolTable();
    // The tree must be freed before the SelectionData objects, since the
    // tree may hold references to the position data in SelectionData.
    sc_.root.reset();
    sc_.sel.clear();
    for (int i = 0; i < sc_.nvars; ++i)
    {
        sfree(sc_.varstrs[i]);
    }
    sfree(sc_.varstrs);
    gmx_ana_index_deinit(&sc_.gall);
    if (sc_.mempool)
    {
        _gmx_sel_mempool_destroy(sc_.mempool);
    }
}


void
SelectionCollection::Impl::clearSymbolTable()
{
    sc_.symtab.reset();
}


namespace
{

/*! \brief
 * Reads a single selection line from stdin.
 *
 * \param[in]  infile        File to read from (typically File::standardInput()).
 * \param[in]  bInteractive  Whether to print interactive prompts.
 * \param[out] line          The read line in stored here.
 * \returns true if something was read, false if at end of input.
 *
 * Handles line continuation, reading also the continuing line(s) in one call.
 */
bool promptLine(File *infile, bool bInteractive, std::string *line)
{
    if (bInteractive)
    {
        fprintf(stderr, "> ");
    }
    if (!infile->readLine(line))
    {
        return false;
    }
    while (endsWith(*line, "\\\n"))
    {
        line->resize(line->length() - 2);
        if (bInteractive)
        {
            fprintf(stderr, "... ");
        }
        std::string buffer;
        // Return value ignored, buffer remains empty and works correctly
        // if there is nothing to read.
        infile->readLine(&buffer);
        line->append(buffer);
    }
    if (endsWith(*line, "\n"))
    {
        line->resize(line->length() - 1);
    }
    else if (bInteractive)
    {
        fprintf(stderr, "\n");
    }
    return true;
}

/*! \brief
 * Helper function for tokenizing the input and pushing them to the parser.
 *
 * \param     scanner       Tokenizer data structure.
 * \param     parserState   Parser data structure.
 * \param[in] bInteractive  Whether to operate in interactive mode.
 *
 * Repeatedly reads tokens using \p scanner and pushes them to the parser with
 * \p parserState until there is no more input, or until enough input is given
 * (only in interactive mode).
 */
int runParserLoop(yyscan_t scanner, _gmx_sel_yypstate *parserState,
                  bool bInteractive)
{
    int status    = YYPUSH_MORE;
    int prevToken = 0;
    do
    {
        YYSTYPE value;
        int     token = _gmx_sel_yylex(&value, scanner);
        if (bInteractive)
        {
            if (token == 0)
            {
                break;
            }
            // Empty commands cause the interactive parser to print out
            // status information. This avoids producing those unnecessarily,
            // e.g., from "resname RA;;".
            if (prevToken == CMD_SEP && token == CMD_SEP)
            {
                continue;
            }
            prevToken = token;
        }
        status = _gmx_sel_yypush_parse(parserState, token, &value, scanner);
    }
    while (status == YYPUSH_MORE);
    _gmx_sel_lexer_rethrow_exception_if_occurred(scanner);
    return status;
}

/*! \brief
 * Helper function that runs the parser once the tokenizer has been
 * initialized.
 *
 * \param[in,out] scanner Scanner data structure.
 * \param[in]     bStdIn  Whether to use a line-based reading
 *      algorithm designed for interactive input.
 * \param[in]     maxnr   Maximum number of selections to parse
 *      (if -1, parse as many as provided by the user).
 * \returns       Vector of parsed selections.
 * \throws        std::bad_alloc if out of memory.
 * \throws        InvalidInputError if there is a parsing error.
 *
 * Used internally to implement parseFromStdin(), parseFromFile() and
 * parseFromString().
 */
SelectionList runParser(yyscan_t scanner, bool bStdIn, int maxnr)
{
    boost::shared_ptr<void>  scannerGuard(scanner, &_gmx_sel_free_lexer);
    gmx_ana_selcollection_t *sc = _gmx_sel_lexer_selcollection(scanner);

    MessageStringCollector   errors;
    _gmx_sel_set_lexer_error_reporter(scanner, &errors);

    int  oldCount = sc->sel.size();
    bool bOk      = false;
    {
        boost::shared_ptr<_gmx_sel_yypstate> parserState(
                _gmx_sel_yypstate_new(), &_gmx_sel_yypstate_delete);
        if (bStdIn)
        {
            File       &stdinFile(File::standardInput());
            bool        bInteractive = _gmx_sel_is_lexer_interactive(scanner);
            std::string line;
            int         status;
            while (promptLine(&stdinFile, bInteractive, &line))
            {
                line.append("\n");
                _gmx_sel_set_lex_input_str(scanner, line.c_str());
                status = runParserLoop(scanner, parserState.get(), true);
                if (status != YYPUSH_MORE)
                {
                    // TODO: Check if there is more input, and issue an
                    // error/warning if some input was ignored.
                    goto early_termination;
                }
                if (!errors.isEmpty() && bInteractive)
                {
                    fprintf(stderr, "%s", errors.toString().c_str());
                    errors.clear();
                }
            }
            status = _gmx_sel_yypush_parse(parserState.get(), 0, NULL,
                                           scanner);
            _gmx_sel_lexer_rethrow_exception_if_occurred(scanner);
early_termination:
            bOk = (status == 0);
        }
        else
        {
            int status = runParserLoop(scanner, parserState.get(), false);
            bOk = (status == 0);
        }
    }
    scannerGuard.reset();
    int nr = sc->sel.size() - oldCount;
    if (maxnr > 0 && nr != maxnr)
    {
        bOk = false;
        errors.append("Too few selections provided");
    }

    // TODO: Remove added selections from the collection if parsing failed?
    if (!bOk || !errors.isEmpty())
    {
        GMX_ASSERT(!bOk && !errors.isEmpty(), "Inconsistent error reporting");
        GMX_THROW(InvalidInputError(errors.toString()));
    }

    SelectionList                     result;
    SelectionDataList::const_iterator i;
    result.reserve(nr);
    for (i = sc->sel.begin() + oldCount; i != sc->sel.end(); ++i)
    {
        result.push_back(Selection(i->get()));
    }
    return result;
}

}   // namespace


void SelectionCollection::Impl::resolveExternalGroups(
        const SelectionTreeElementPointer &root,
        MessageStringCollector            *errors)
{

    if (root->type == SEL_GROUPREF)
    {
        bool bOk = true;
        if (grps_ == NULL)
        {
            // TODO: Improve error messages
            errors->append("Unknown group referenced in a selection");
            bOk = false;
        }
        else if (root->u.gref.name != NULL)
        {
            char *name = root->u.gref.name;
            if (!gmx_ana_indexgrps_find(&root->u.cgrp, grps_, name))
            {
                // TODO: Improve error messages
                errors->append("Unknown group referenced in a selection");
                bOk = false;
            }
            else
            {
                sfree(name);
            }
        }
        else
        {
            if (!gmx_ana_indexgrps_extract(&root->u.cgrp, grps_,
                                           root->u.gref.id))
            {
                // TODO: Improve error messages
                errors->append("Unknown group referenced in a selection");
                bOk = false;
            }
        }
        if (bOk)
        {
            root->type = SEL_CONST;
            root->setName(root->u.cgrp.name);
        }
    }

    SelectionTreeElementPointer child = root->child;
    while (child)
    {
        resolveExternalGroups(child, errors);
        child = child->next;
    }
}


/********************************************************************
 * SelectionCollection
 */

SelectionCollection::SelectionCollection()
    : impl_(new Impl)
{
}


SelectionCollection::~SelectionCollection()
{
}


void
SelectionCollection::initOptions(Options *options)
{
    static const char * const debug_levels[]
        = {"no", "basic", "compile", "eval", "full", NULL};
    /*
       static const char * const desc[] = {
        "This program supports selections in addition to traditional",
        "index files. Use [TT]-select help[tt] for additional information,",
        "or type 'help' in the selection prompt.",
        NULL,
       };
       options.setDescription(desc);
     */

    const char *const *postypes = PositionCalculationCollection::typeEnumValues;
    options->addOption(StringOption("selrpos").enumValue(postypes)
                           .store(&impl_->rpost_).defaultValue(postypes[0])
                           .description("Selection reference positions"));
    options->addOption(StringOption("seltype").enumValue(postypes)
                           .store(&impl_->spost_).defaultValue(postypes[0])
                           .description("Default selection output positions"));
    GMX_RELEASE_ASSERT(impl_->debugLevel_ >= 0 && impl_->debugLevel_ <= 4,
                       "Debug level out of range");
    options->addOption(StringOption("seldebug").hidden(impl_->debugLevel_ == 0)
                           .enumValue(debug_levels)
                           .defaultValue(debug_levels[impl_->debugLevel_])
                           .storeEnumIndex(&impl_->debugLevel_)
                           .description("Print out selection trees for debugging"));
}


void
SelectionCollection::setReferencePosType(const char *type)
{
    GMX_RELEASE_ASSERT(type != NULL, "Cannot assign NULL position type");
    // Check that the type is valid, throw if it is not.
    e_poscalc_t  dummytype;
    int          dummyflags;
    PositionCalculationCollection::typeFromEnum(type, &dummytype, &dummyflags);
    impl_->rpost_ = type;
}


void
SelectionCollection::setOutputPosType(const char *type)
{
    GMX_RELEASE_ASSERT(type != NULL, "Cannot assign NULL position type");
    // Check that the type is valid, throw if it is not.
    e_poscalc_t  dummytype;
    int          dummyflags;
    PositionCalculationCollection::typeFromEnum(type, &dummytype, &dummyflags);
    impl_->spost_ = type;
}


void
SelectionCollection::setDebugLevel(int debugLevel)
{
    impl_->debugLevel_ = debugLevel;
}


void
SelectionCollection::setTopology(t_topology *top, int natoms)
{
    GMX_RELEASE_ASSERT(natoms > 0 || top != NULL,
                       "The number of atoms must be given if there is no topology");
    // Get the number of atoms from the topology if it is not given.
    if (natoms <= 0)
    {
        natoms = top->atoms.nr;
    }
    gmx_ana_selcollection_t *sc = &impl_->sc_;
    // Do this first, as it allocates memory, while the others don't throw.
    gmx_ana_index_init_simple(&sc->gall, natoms, NULL);
    sc->pcc.setTopology(top);
    sc->top = top;
}


void
SelectionCollection::setIndexGroups(gmx_ana_indexgrps_t *grps)
{
    GMX_RELEASE_ASSERT(grps == NULL || !impl_->bExternalGroupsSet_,
                       "Can only set external groups once or clear them afterwards");
    impl_->grps_               = grps;
    impl_->bExternalGroupsSet_ = true;

    MessageStringCollector      errors;
    SelectionTreeElementPointer root = impl_->sc_.root;
    while (root)
    {
        impl_->resolveExternalGroups(root, &errors);
        root = root->next;
    }
    if (!errors.isEmpty())
    {
        GMX_THROW(InvalidInputError(errors.toString()));
    }
}


bool
SelectionCollection::requiresTopology() const
{
    e_poscalc_t  type;
    int          flags;

    if (!impl_->rpost_.empty())
    {
        flags = 0;
        // Should not throw, because has been checked earlier.
        PositionCalculationCollection::typeFromEnum(impl_->rpost_.c_str(),
                                                    &type, &flags);
        if (type != POS_ATOM)
        {
            return true;
        }
    }
    if (!impl_->spost_.empty())
    {
        flags = 0;
        // Should not throw, because has been checked earlier.
        PositionCalculationCollection::typeFromEnum(impl_->spost_.c_str(),
                                                    &type, &flags);
        if (type != POS_ATOM)
        {
            return true;
        }
    }

    SelectionTreeElementPointer sel = impl_->sc_.root;
    while (sel)
    {
        if (_gmx_selelem_requires_top(*sel))
        {
            return true;
        }
        sel = sel->next;
    }
    return false;
}


SelectionList
SelectionCollection::parseFromStdin(int nr, bool bInteractive)
{
    yyscan_t scanner;

    _gmx_sel_init_lexer(&scanner, &impl_->sc_, bInteractive, nr,
                        impl_->bExternalGroupsSet_,
                        impl_->grps_);
    return runParser(scanner, true, nr);
}


SelectionList
SelectionCollection::parseFromFile(const std::string &filename)
{

    try
    {
        yyscan_t scanner;
        File     file(filename, "r");
        // TODO: Exception-safe way of using the lexer.
        _gmx_sel_init_lexer(&scanner, &impl_->sc_, false, -1,
                            impl_->bExternalGroupsSet_,
                            impl_->grps_);
        _gmx_sel_set_lex_input_file(scanner, file.handle());
        return runParser(scanner, false, -1);
    }
    catch (GromacsException &ex)
    {
        ex.prependContext(formatString(
                                  "Error in parsing selections from file '%s'",
                                  filename.c_str()));
        throw;
    }
}


SelectionList
SelectionCollection::parseFromString(const std::string &str)
{
    yyscan_t scanner;

    _gmx_sel_init_lexer(&scanner, &impl_->sc_, false, -1,
                        impl_->bExternalGroupsSet_,
                        impl_->grps_);
    _gmx_sel_set_lex_input_str(scanner, str.c_str());
    return runParser(scanner, false, -1);
}


void
SelectionCollection::compile()
{
    if (impl_->sc_.top == NULL && requiresTopology())
    {
        GMX_THROW(InconsistentInputError("Selection requires topology information, but none provided"));
    }
    if (!impl_->bExternalGroupsSet_)
    {
        setIndexGroups(NULL);
    }
    if (impl_->debugLevel_ >= 1)
    {
        printTree(stderr, false);
    }

    SelectionCompiler compiler;
    compiler.compile(this);

    if (impl_->debugLevel_ >= 1)
    {
        std::fprintf(stderr, "\n");
        printTree(stderr, false);
        std::fprintf(stderr, "\n");
        impl_->sc_.pcc.printTree(stderr);
        std::fprintf(stderr, "\n");
    }
    impl_->sc_.pcc.initEvaluation();
    if (impl_->debugLevel_ >= 1)
    {
        impl_->sc_.pcc.printTree(stderr);
        std::fprintf(stderr, "\n");
    }
}


void
SelectionCollection::evaluate(t_trxframe *fr, t_pbc *pbc)
{
    impl_->sc_.pcc.initFrame();

    SelectionEvaluator evaluator;
    evaluator.evaluate(this, fr, pbc);

    if (impl_->debugLevel_ >= 3)
    {
        std::fprintf(stderr, "\n");
        printTree(stderr, true);
    }
}


void
SelectionCollection::evaluateFinal(int nframes)
{
    SelectionEvaluator evaluator;
    evaluator.evaluateFinal(this, nframes);
}


void
SelectionCollection::printTree(FILE *fp, bool bValues) const
{
    SelectionTreeElementPointer sel = impl_->sc_.root;
    while (sel)
    {
        _gmx_selelem_print_tree(fp, *sel, bValues, 0);
        sel = sel->next;
    }
}


void
SelectionCollection::printXvgrInfo(FILE *out, output_env_t oenv) const
{
    if (output_env_get_xvg_format(oenv) != exvgNONE)
    {
        const gmx_ana_selcollection_t &sc = impl_->sc_;
        std::fprintf(out, "# Selections:\n");
        for (int i = 0; i < sc.nvars; ++i)
        {
            std::fprintf(out, "#   %s\n", sc.varstrs[i]);
        }
        for (size_t i = 0; i < sc.sel.size(); ++i)
        {
            std::fprintf(out, "#   %s\n", sc.sel[i]->selectionText());
        }
        std::fprintf(out, "#\n");
    }
}

// static
HelpTopicPointer
SelectionCollection::createDefaultHelpTopic()
{
    return createSelectionHelpTopic();
}

} // namespace gmx
