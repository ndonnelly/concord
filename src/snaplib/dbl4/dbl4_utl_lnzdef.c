/*************************************************************************
**
**  Filename:    %M%
**
**  Version:     %I%
**
**  What string: %W%
**
** $Id: dbl4_utl_lnzdef.c,v 1.3 2005/04/04 23:57:58 ccrook Exp $
**//**
** \file
**      Functions for evaluating the complete LINZ deformation model
**      accessed via a binary source object.
**
**      The deformation model is defined by one or more sequences each
**      comprising one or more components.  The sequences define deformation
**      for a specific period and geographical extent.  Each sequence can
**      either define deformation (displacement) or velocities, and can define
**      it in one, two, or three dimensions.  The components are either grid
**      or triangulation definitions of displacement, which are embedded into
**      the binary file.  For velocity models each component is added
**      independently.  For displacement models the deformation is interpolated
**      between the two models that apply (one before, one after), or
**      extrapolated from the first or last two components.  Alternatively
**      the displacement may be defined as fixed or zero before or after
**      each component.
**
**      The main components used in this code are:
**         DefMod      The deformation model
**         DefSeq      A deformation sequence in the model
**         DefCmp      A component in the sequence
**
**      The binary format is portable between SUN and Intel DOS/Windows
**      environments which differ only in endianness
**
*************************************************************************
*/

static char sccsid[] = "%W%";

#include "dbl4_common.h"

#include <stdlib.h>
#include <string.h>

#include "dbl4_utl_lnzdef.h"
#include "dbl4_utl_trig.h"
#include "dbl4_utl_grid.h"
#include "dbl4_utl_date.h"

#include "dbl4_utl_alloc.h"
#include "dbl4_utl_error.h"

/* Note: code below assumes headers are all the same length */

#define LNZDEF_FILE_HEADER_1 "LINZ deformation model v1.0L\r\n\x1A"
#define LNZDEF_FILE_HEADER_2 "LINZ deformation model v1.0B\r\n\x1A"

typedef enum { defModelGrid, defModelTrig } DefModelType;
typedef enum { defEvalZero, defEvalFixed, defEvalInterp } DefEvalMode;
typedef enum { defValueDeformation, defValueVelocity } DefValueType;

typedef struct
{
    double xmin;
    double ymin;
    double xmax;
    double ymax;
} CrdRange, *hCrdRange;

typedef struct s_DefCmp
{
    int id;
    char *description;
    DateTimeType refdate;
    CrdRange range;
    int dimension;
    DefEvalMode beforemode;
    DefEvalMode aftermode;
    DefModelType type;
    INT4 offset;
    hBinSrc refbinsrc;
    hBinSrc binsrc;
    union
    {
        hGrid grid;
        hTrig trig;
    } model;
    Boolean loaded;
    StatusType loadstatus;
    struct s_DefCmp *nextcmp;
} DefCmp, *hDefCmp;

typedef struct s_DefSeq
{
    int id;
    char *name;
    char *description;
    DateTimeType startdate;
    DateTimeType enddate;
    CrdRange range;
    int dimension;
    DefValueType valtype;
    Boolean zerobeyond;
    hDefCmp firstcmp;
    hDefCmp lastcmp;
    struct s_DefSeq *nextseq;
} DefSeq, *hDefSeq;

typedef struct
{
    char *name;
    char *version;
    char *crdsyscode;
    char *description;
    DateTimeType versiondate;
    DateTimeType startdate;
    DateTimeType enddate;
    CrdRange range;
    Boolean isgeographical;
    int nsequences;
    hDefSeq firstseq;
    hDefSeq lastseq;
    hBinSrc binsrc;
} DefMod, *hDefMod;



/*************************************************************************
** Function name: check_header
**//**
**    Reads the binary source header and checks that it is compatible with the
**    one of the defined valid headers, each defining a version of the linz
**    deformation model format.  Also sets the binary source endian-ness to
**    match the model and retrieves the location of the deformation model
**    index data in the data stream.
**
**  \param binsrc              The binary data source to read from
**  \param indexloc            Returns the location of the grid index
**                             data
**
**  \return                    Returns the grid format version number
**
**************************************************************************
*/

static int check_header( hBinSrc binsrc, INT4 *indexloc)
{
    char buf[80];
    INT4 len;
    int version;
    int big_endian=0;
    version = 0;
    len = strlen( LNZDEF_FILE_HEADER_1 );
    if( utlBinSrcLoad1( binsrc, 0, len, buf ) != STS_OK ) return 0;

    if(  memcmp( buf, LNZDEF_FILE_HEADER_1, len ) == 0 )
    {
        version = 1;
        big_endian = 0;
    }
    else if(  memcmp( buf, LNZDEF_FILE_HEADER_2, len ) == 0 )
    {
        version = 1;
        big_endian = 1;
    }
    utlBinSrcSetBigEndian( binsrc, big_endian );

    if( utlBinSrcLoad4( binsrc, BINSRC_CONTINUE, 1, indexloc ) != STS_OK ) return
            0;
    if( ! *indexloc ) version = 0;
    return version;
}


/*************************************************************************
** Function name: delete_def_comp
**//**
**    Deletes a deformation component, releasing any resources that
**    it uses.
**
**  \param cmp                 The deformation component to delete
**
**  \return
**
**************************************************************************
*/

static void delete_def_comp( hDefCmp cmp )
{
    if( cmp->description )
    {
        utlFree( cmp->description );
        cmp->description = NULL;
    }
    /*> If a grid or triangulation model has been loaded to calculate the
        component, then release it with utlReleaseGrid or utlReleaseTrig */

    if( cmp->loaded )
    {
        switch (cmp->type)
        {
        case defModelGrid:
            if( cmp->model.grid )
            {
                utlReleaseGrid( cmp->model.grid );
                cmp->model.grid = NULL;
            }
            break;
        case defModelTrig:
            if( cmp->model.trig )
            {
                utlReleaseTrig( cmp->model.trig );
                cmp->model.trig = NULL;
            }
            break;
        };
    };
    if( cmp->binsrc )
    {
        utlReleaseBinSrc(cmp->binsrc);
        cmp->binsrc = 0;
    }

    /*> Release memory allocated to the component */
    utlFree( cmp );
}


/*************************************************************************
** Function name: delete_def_seq
**//**
**    Deletes a deformation sequence and releases any resources allocated
**    to it.  In particular this also deletes any components that it
**    includes
**
**  \param seq                 The deformation sequence to delete.
**
**  \return
**
**************************************************************************
*/

static void delete_def_seq( hDefSeq seq )
{
    hDefCmp cmp;
    hDefCmp nextcmp;

    /*> Delete any string resources */
    if( seq->name )
    {
        utlFree( seq->name );
        seq->name = NULL;
    }
    if( seq->description )
    {
        utlFree( seq->description );
        seq->description = NULL;
    }

    /*> Delete all deformation components with delete_def_comp */
    cmp  = seq->firstcmp;
    while( cmp )
    {
        nextcmp = cmp->nextcmp;
        delete_def_comp( cmp );
        cmp = nextcmp;
    }
    seq->firstcmp = NULL;
    seq->lastcmp = NULL;

    /*> Release memory allocated to the sequence object */
    utlFree( seq );
}


/*************************************************************************
** Function name: delete_def_mod
**//**
**    Deletes a deformation model and releases any resources it uses.
**    In particular it deletes any deformation sequences defined for the
**    model.
**
**  \param mod                 The deformation model to delete
**
**  \return
**
**************************************************************************
*/

static void delete_def_mod( hDefMod mod )
{
    hDefSeq seq, nextseq;

    /*> Deallocate string resources */

    if( mod->name )
    {
        utlFree(mod->name);
        mod->name = NULL;
    }
    if( mod->version )
    {
        utlFree(mod->version);
        mod->version = NULL;
    }
    if( mod->crdsyscode )
    {
        utlFree(mod->crdsyscode);
        mod->crdsyscode = NULL;
    }
    if( mod->description )
    {
        utlFree(mod->description);
        mod->description = NULL;
    }

    /*> Delete each deformation sequence with delete_def_seq */

    seq = mod->firstseq;
    while( seq )
    {
        nextseq = seq->nextseq;
        delete_def_seq( seq );
        seq = nextseq;
    }
    mod->firstseq = NULL;
    mod->lastseq = NULL;

    /*> Release the model itself */

    utlFree( mod );
}


/*************************************************************************
** Function name: load_date
**//**
**    Function to load a date from the blob.  The date is stored as
**    2 byte year, month (Jan=1), day, hour, minute, second.
**
**  \param binsrc              The binary source object
**  \param offset              The location from which the date
**                             is to be read
**  \param date                The date to be returned
**
**  \return                    The return status
**
**************************************************************************
*/

static StatusType load_date( hBinSrc binsrc, long offset, DateTimeType *date )
{
    INT2 datecomp[6];
    StatusType sts;
    sts = utlBinSrcLoad2( binsrc, offset, 6, (void *) (datecomp) );

    if( sts != STS_OK ) RETURN_STATUS( sts );

    utlSetDateTime( date, datecomp[0], datecomp[1], datecomp[2],
                    datecomp[3], datecomp[4], datecomp[5] );
    return STS_OK;
}


/*************************************************************************
** Function name: load_component
**//**
**    Creates and loads a deformation component
**
**    Note: this doesn't read the grid or trig model itself, just the
**    definition of it.  The actual model is loaded only when it is first
**    required.
**
**  \param binsrc              The binary source from which to load
**                             the component
**  \param pcmp                The component object that is created.
**
**  \return                    The return status
**
**************************************************************************
*/


static StatusType load_component( hBinSrc binsrc, hDefCmp *pcmp )
{
    hDefCmp cmp;
    StatusType sts;

    INT2 usebefore=0;
    INT2 useafter=0;
    INT2 istrig=0;

    /*> Allocate a DefCmp object */

    (*pcmp) = NULL;
    cmp = (hDefCmp) utlAlloc( sizeof( DefCmp ) );
    if( ! cmp ) RETURN_STATUS(STS_ALLOC_FAILED);

    TRACE_LNZDEF(("Loading component"));

    cmp->id = 0;
    cmp->description = NULL;
    cmp->binsrc = NULL;
    cmp->refbinsrc = binsrc;
    cmp->loaded = BLN_FALSE;
    cmp->loadstatus = STS_OK;
    cmp->beforemode = defEvalZero;
    cmp->aftermode = defEvalZero;
    cmp->type = defModelGrid;
    cmp->nextcmp = NULL;
    cmp->dimension = 0;

    /*> Load the object from the binary source file */

    sts = STS_OK;
    if( sts == STS_OK ) sts = utlBinSrcLoadString( binsrc, BINSRC_CONTINUE, &(cmp->description) );
    if( sts == STS_OK ) sts = load_date( binsrc, BINSRC_CONTINUE, &(cmp->refdate) );

    if( sts == STS_OK ) sts = utlBinSrcLoad8( binsrc, BINSRC_CONTINUE, 1, &(cmp->range.ymin) );
    if( sts == STS_OK ) sts = utlBinSrcLoad8( binsrc, BINSRC_CONTINUE, 1, &(cmp->range.ymax) );
    if( sts == STS_OK ) sts = utlBinSrcLoad8( binsrc, BINSRC_CONTINUE, 1, &(cmp->range.xmin) );
    if( sts == STS_OK ) sts = utlBinSrcLoad8( binsrc, BINSRC_CONTINUE, 1, &(cmp->range.xmax) );

    if( sts == STS_OK ) sts = utlBinSrcLoad2( binsrc, BINSRC_CONTINUE, 1, &usebefore );
    if( usebefore == 1 ) cmp->beforemode = defEvalFixed;
    if( usebefore == 2 ) cmp->beforemode = defEvalInterp;

    if( sts == STS_OK ) sts = utlBinSrcLoad2( binsrc, BINSRC_CONTINUE, 1, &useafter );
    if( useafter == 1 ) cmp->aftermode = defEvalFixed;
    if( useafter == 2 ) cmp->aftermode = defEvalInterp;

    if( sts == STS_OK ) sts = utlBinSrcLoad2( binsrc, BINSRC_CONTINUE, 1, &istrig );
    if( istrig ) cmp->type = defModelTrig;

    if( sts == STS_OK ) sts = utlBinSrcLoad4( binsrc, BINSRC_CONTINUE, 1, &(cmp->offset) );

    /*> If the load was not successful, delete the partially loaded object with
        delete_def_comp */

    if( sts != STS_OK )
    {
        delete_def_comp( cmp );
        RETURN_STATUS( sts );
    }

    TRACE_LNZDEF(("Component %s loaded",
                  cmp->description ? cmp->description : "(No description)"));

    (*pcmp) = cmp;

    return sts;
}


/*************************************************************************
** Function name: load_component_model
**//**
**    Loads the actual deformation/velocity  model from the component
**
**  \param cmp                 The definition of the deformation
**                             component.
**
**  \return                    The return status
**
**************************************************************************
*/

static StatusType load_component_model( hDefCmp cmp )
{
    StatusType sts;

    /*> If the model has already been loaded then return success */

    if( cmp->loaded ) return STS_OK;

    /*> If the model has already tried to load and failed, then return failure */

    if( cmp->loadstatus != STS_OK ) RETURN_STATUS( cmp->loadstatus );

    TRACE_LNZDEF(("Loading component model %s at offset %d",cmp->description,cmp->offset));

    /*> Create a new binary source object from the original source with the
        model offset applied using utlCreateEmbeddedBinSrc */

    sts = utlCreateEmbeddedBinSrc( cmp->refbinsrc, cmp->offset, &(cmp->binsrc) );
    if( sts == STS_OK )
    {

        /*> Depending upon the model type, create the model with utlCreateGrid
            or utlCreateTrig */

        switch (cmp->type)
        {
        case defModelGrid:
            sts = utlCreateGrid( cmp->binsrc, &(cmp->model.grid) );
            break;
        case defModelTrig:
            sts = utlCreateTrig( cmp->binsrc, &(cmp->model.trig) );
            break;
        default:
            sts = STS_INVALID_DATA;
            break;
        };
    };

    /*> Record the load status if not successful */

    if( sts != STS_OK )
    {
        cmp->loadstatus = sts;
        RETURN_STATUS( sts );
    };

    cmp->loaded = BLN_TRUE;

    TRACE_LNZDEF(("Component model %s loaded",cmp->description));

    return sts;
}



/*************************************************************************
** Function name: calc_component_model
**//**
**    Routine to calculate the model value for the component at a specified
**    location
**
**  \param cmp                 The component to evaluate
**  \param x                   The x coordinate
**  \param y                   The y coordinate
**  \param result              Array to store the results
**
**  \return                    The return status
**
**************************************************************************
*/

static StatusType calc_component_model( hDefCmp cmp, double x, double y, double *result )
{
    StatusType sts = STS_OK;

    /*> Load the model if it is not already loaded */

    if( ! cmp->loaded )
    {
        sts = load_component_model( cmp );
    }


    /*> Evaluate the model with utlCalcGridLinear or utlCalcTrig, depending
        upon the model type */

    if( sts == STS_OK )
    {
        switch (cmp->type)
        {
        case defModelGrid:
            sts = utlCalcGridLinear( cmp->model.grid, x, y, result );
            break;
        case defModelTrig:
            sts = utlCalcTrig( cmp->model.trig, x, y, result );
            break;
        default:
            sts = STS_INVALID_DATA;
            break;
        };
    };

    if( sts != STS_OK ) RETURN_STATUS( sts );
    return sts;
}


/*************************************************************************
** Function name: load_sequence
**//**
**    Creates and loads the deformation sequence object from a binary stream.
**
**  \param binsrc              The binary source object
**  \param pseq                Returns the deformation sequence
**                             object that is created.
**
**  \return                    The return status
**
**************************************************************************
*/

static StatusType load_sequence( hBinSrc binsrc, hDefSeq *pseq )
{
    hDefSeq seq;
    StatusType sts;
    INT2 isvel=0;
    INT2 dimension=0;
    INT2 zerobeyond=0;
    INT2 ncomponents=0;
    int idcomp;

    TRACE_LNZDEF(("Loading deformation component sequence"));

    /*> Allocate the DefSeq object */

    (*pseq) = NULL;
    seq = (hDefSeq) utlAlloc( sizeof( DefSeq ) );
    if( ! seq ) RETURN_STATUS(STS_ALLOC_FAILED);

    /*> Initiallize resource pointers in the object */

    seq->id = 0;
    seq->name = NULL;
    seq->description = NULL;
    seq->firstcmp = NULL;
    seq->lastcmp = NULL;
    seq->nextseq = NULL;

    sts = STS_OK;

    /*> Load the sequence data from the binary source.  Load name, description,
        start and end dates, coordinate range, and dimension and model value
        type */

    if( sts == STS_OK ) sts = utlBinSrcLoadString( binsrc, BINSRC_CONTINUE, &(seq->name) );
    if( sts == STS_OK ) sts = utlBinSrcLoadString( binsrc, BINSRC_CONTINUE, &(seq->description) );
    if( sts == STS_OK ) sts = load_date( binsrc, BINSRC_CONTINUE, &(seq->startdate) );
    if( sts == STS_OK ) sts = load_date( binsrc, BINSRC_CONTINUE, &(seq->enddate) );

    if( sts == STS_OK ) sts = utlBinSrcLoad8( binsrc, BINSRC_CONTINUE, 1, &(seq->range.ymin) );
    if( sts == STS_OK ) sts = utlBinSrcLoad8( binsrc, BINSRC_CONTINUE, 1, &(seq->range.ymax) );
    if( sts == STS_OK ) sts = utlBinSrcLoad8( binsrc, BINSRC_CONTINUE, 1, &(seq->range.xmin) );
    if( sts == STS_OK ) sts = utlBinSrcLoad8( binsrc, BINSRC_CONTINUE, 1, &(seq->range.xmax) );

    if( sts == STS_OK ) sts = utlBinSrcLoad2( binsrc, BINSRC_CONTINUE, 1, &isvel );
    seq->valtype = isvel ? defValueVelocity : defValueDeformation;

    if( sts == STS_OK ) sts = utlBinSrcLoad2( binsrc, BINSRC_CONTINUE, 1, &dimension );
    seq->dimension = dimension;

    if( sts == STS_OK ) sts = utlBinSrcLoad2( binsrc, BINSRC_CONTINUE, 1, &zerobeyond );
    seq->zerobeyond = zerobeyond ? BLN_TRUE : BLN_FALSE;

    if( sts == STS_OK ) sts = utlBinSrcLoad2( binsrc, BINSRC_CONTINUE, 1, &ncomponents );

    TRACE_LNZDEF(("Deformation component sequence header loaded: %s",seq->name));

    /*> Load the each of the sequence components with load_component */

    idcomp = 0;
    while( sts == STS_OK && ncomponents-- )
    {
        hDefCmp cmp;
        sts = load_component( binsrc, &cmp );
        if( sts == STS_OK )
        {
            idcomp++;
            cmp->id = idcomp;
            cmp->dimension = dimension;
            if( ! seq->firstcmp ) seq->firstcmp = cmp;
            if( seq->lastcmp ) seq->lastcmp->nextcmp = cmp;
            seq->lastcmp = cmp;
        }
    }

    /* If the sequence is not successfully loaded then delete the sequence
       resources that have been allocated with delete_def_seq */

    if( sts != STS_OK )
    {
        delete_def_seq( seq );
        RETURN_STATUS(sts);
    }

    TRACE_LNZDEF(("Deformation sequence components loaded: %s",seq->name));

    (*pseq) = seq;
    return sts;
}



/*************************************************************************
** Function name: calc_seq_def
**//**
**    Calculates the displacement for a deformation sequence at a specific
**    location and time.  This is used where the sequence is a deformation
**    model (ie not a velocity model)
**
**  \param seq                 The sequence object
**  \param date                The date (in years) at which to
**                             evaluate the displacement
**  \param x                   The x coordinate
**  \param y                   The y coordinate
**  \param value               Array returning the calculated values
**
**  \return                    The return status
**
**************************************************************************
*/

static StatusType calc_seq_def( hDefSeq seq, double date, double x, double y, double *value )
{

    hDefCmp cmp0;
    double dateoffset0=0.0;
    double factor0=0.0;
    hDefCmp cmp1;
    double dateoffset1;
    double factor1=0.0;
    double cval[3];
    StatusType sts;
    int i;

    cval[0] = cval[1] = cval[2] = 0;

    for( i = 0; i < seq->dimension; i++ )
    {
        value[i] = 0.0;
    }

    /*> Search through the components of the sequence to identify the
        components before and after the evaluation date.  This may end
        up finding the first or last two elements if the date is outside
        the range of deformation models.  */

    cmp0 = NULL;
    cmp1 = seq->firstcmp;
    dateoffset1 = date - utlDateAsYear( &(cmp1->refdate) );
    while(cmp1->nextcmp)
    {
        cmp0 = cmp1;
        dateoffset0 = dateoffset1;
        cmp1 = cmp1->nextcmp;
        dateoffset1 = date - utlDateAsYear( &(cmp1->refdate) );
        if( dateoffset1 < 0 ) break;
    }

    /*> Work out either one or two components that will be used to evaluate
        the deformation, and a multiplication factor for each component */

    /*> If there is Only one component in sequence and the component is to be
        evaluated at the required date (ie is not set to zero) then it
        is simply evaluated at that time */

    sts = STS_OK;

    if( ! cmp0 )
    {
        if( (dateoffset1 < 0 && cmp1->beforemode == defEvalZero) ||
                (dateoffset1 >= 0 && cmp1->aftermode == defEvalZero) )
        {
            cmp1 = NULL;
        }
        else
        {
            factor1 = 1.0;
        }
    }

    /*> Else if the date is before the first component, then determine whether
        it is zero, equal to the first component, or interpolated between
        the first two components, and set the components used and multiples
        accordingly. */

    else if( dateoffset0 < 0 )
    {
        switch ( cmp0->beforemode )
        {
        case defEvalZero:
            cmp0 = cmp1 = NULL;
            break;
        case defEvalFixed:
            cmp1 = NULL;
            factor0 = 1.0;
            break;
        case defEvalInterp:
            factor0 = dateoffset1/(dateoffset1-dateoffset0);
            factor1 = 1.0-factor0;
            break;
        default:
            SET_STATUS(sts,STS_INVALID_DATA);
            break;
        };
    }

    /*> Else if the date is after the last component then determine
        it is zero, equal to the last component, or interpolated between
        the last two components */


    else if( dateoffset1 > 0 )
    {
        switch ( cmp1->aftermode )
        {
        case defEvalZero:
            cmp0 = cmp1 = NULL;
            break;
        case defEvalFixed:
            cmp0 = NULL;
            factor1 = 1.0;
            break;
        case defEvalInterp:
            factor0 = dateoffset1/(dateoffset1-dateoffset0);
            factor1 = 1.0-factor0;
            break;
        default:
            SET_STATUS(sts,STS_INVALID_DATA);
            break;
        };
    }

    /*> Otherwise it is between two components, so use the components
        before and after evaluation flags to determine whether the value
        should be zero, equal to the first component, equal to the second
        component, or interpolated between them. */

    else
    {
        if( cmp0->aftermode == defEvalInterp &&
                cmp1->beforemode == defEvalInterp )
        {
            factor0 = dateoffset1/(dateoffset1-dateoffset0);
            factor1 = 1.0-factor0;
        }
        else if( cmp0->aftermode == defEvalFixed &&
                 cmp1->beforemode == defEvalZero )
        {
            factor0 = 1.0;
            cmp1 = NULL;
        }
        else if( cmp0->aftermode == defEvalZero &&
                 cmp1->beforemode == defEvalFixed )
        {
            factor1 = 1.0;
            cmp0 = NULL;
        }
        else if( cmp0->aftermode == defEvalZero &&
                 cmp1->beforemode == defEvalZero )
        {
            cmp0 = cmp1 = NULL;
        }
        else
        {
            SET_STATUS( sts, STS_INVALID_DATA );
        }
    }

    /*> Finally evaluate the 0, 1, or 2 components, scale the calculated
        values by the factor determined, and return the result.  If the
        evaluation fails then return the corresponding failed status.
        The evaluation uses calc_component_model. */

    if( cmp0 )
    {
        StatusType stscmp;
        stscmp = calc_component_model( cmp0, x, y, cval );
        if( stscmp == STS_OK )
        {
            for( i = 0; i < seq->dimension; i++ )
            {
                value[i] += factor0 * cval[i];
            }
            TRACE_LNZDEF(("calc_seq_def: Adding component %d, factor %.4lf, value %.4lf %.4lf %.4lf",
                          cmp0->id,factor0,cval[0],cval[1],cval[2]));
        }
        else
        {
            sts = stscmp;
        }
    }

    if( cmp1 )
    {
        StatusType stscmp;
        stscmp = calc_component_model( cmp1, x, y, cval );
        if( stscmp == STS_OK )
        {
            for( i = 0; i < seq->dimension; i++ )
            {
                value[i] += factor1 * cval[i];
            }
            TRACE_LNZDEF(("calc_seq_def: Adding component %d, factor %.4lf, value %.4lf %.4lf %.4lf",
                          cmp1->id,factor1,cval[0],cval[1],cval[2]));
        }
        else
        {
            sts = stscmp;
        }
    }

    if( sts != STS_OK ) RETURN_STATUS( sts );
    return STS_OK;
}


/*************************************************************************
** Function name: calc_seq_vel
**//**
**    Calculate a velocity deformation sequence at a specific location and time.
**    Returns the displacement evaluated at the specified time.
**
**  \param seq                 The sequence
**  \param date                The date at which it is to be evaluated
**  \param x                   The x coordinate
**  \param y                   The y coordinate
**  \param value               Array to return the resulting offset
**
**  \return                    The return status
**
**************************************************************************
*/

static StatusType calc_seq_vel( hDefSeq seq, double date, double x, double y,
                                double *value )
{

    hDefCmp cmp;
    DefEvalMode mode;
    double cval[3];
    int i;
    StatusType sts = STS_OK;
    StatusType stscmp;

    cval[0] = cval[1] = cval[2] = 0.0;

    /*> Initiallize the results array to zero */

    for( i = 0; i < seq->dimension; i++ )
    {
        value[i] = 0.0;
    }

    /*> For each component in the sequence determine whether the evaluation
        date is before or after the reference date for the component, then
        look at the corresponding evaluation mode (before or after) to determine
        whether the deformation is to be evaluated.  If it is then evaluate the
        component velocity with calc_component_model, multiply by the time
        difference, and add the resulting displacement to the results array.
        */

    for( cmp = seq->firstcmp; cmp; cmp = cmp->nextcmp )
    {
        double dateoffset = date - utlDateAsYear( &(cmp->refdate) );
        mode =  dateoffset <= 0 ? cmp->beforemode : cmp->aftermode;
        if( mode == defEvalZero ) continue;
        stscmp = calc_component_model( cmp, x, y, cval );
        if( stscmp == STS_OK )
        {
            for( i = 0; i < seq->dimension; i++ )
            {
                value[i] += dateoffset * cval[i];
            }
        }
        else
        {
            sts = stscmp;
        }
        TRACE_LNZDEF(("calc_seq_val: Adding component %d, factor %.4f, value %.4f %.4f %.4f",
                      cmp->id, dateoffset, cval[0], cval[1], cval[2] ));
    }
    if( sts != STS_OK ) RETURN_STATUS( sts );
    return sts;
}


/*************************************************************************
** Function name: check_range
**//**
**    Function to check whether a specified point is inside or outside
**    a coordinate range.
**
**  \param range               The coordinate range
**  \param x                   The x coordinate
**  \param y                   The y coordinate
**
**  \return                    The return status
**
**************************************************************************
*/

static StatusType check_range( hCrdRange range, double x, double y )
{
    if( x < range->xmin || x > range->xmax ||
            y < range->ymin || y > range->ymax ) RETURN_STATUS( STS_CRD_RANGE_ERR );
    return STS_OK;
}


/*************************************************************************
** Function name: add_sequence
**//**
**    Adds the displacement from a deformation sequence to that for the total
**    model
**
**  \param seq                 The deformation sequence
**  \param date                The evaluation date
**  \param x                   The x coordinate
**  \param y                   The y coordinate
**  \param value               The array to which the displacement is
**                             to be added.
**
**  \return                    The return status
**
**************************************************************************
*/

static StatusType add_sequence( hDefSeq seq, double date, double x,
                                double y, double* value )
{
    StatusType sts;
    double seqval[3];

    TRACE_LNZDEF(("add_sequence: Adding offset from sequence %d",seq->id));

    seqval[0] = 0.0;
    seqval[1] = 0.0;
    seqval[2] = 0.0;

    /*> If the date is not within the range specified by the model then
        return without doing anything */

    if( date < utlDateAsYear( &(seq->startdate) ) ||
            date > utlDateAsYear( &(seq->enddate) ) ) return STS_OK;

    /*> Check that the coordinates are within the range of the sequence
        with calc_range.

        If they are then evaluate the displacement for the deformation
        sequence using calc_seq_def (for deformation sequences) or
        calc_seq_vel (for velocity sequences).
        */

    sts = check_range( &(seq->range), x, y );

    if( sts == STS_OK )
    {
        switch( seq->valtype )
        {
        case defValueDeformation:
            sts = calc_seq_def( seq, date, x, y, seqval );
            break;
        case defValueVelocity:
            sts = calc_seq_vel( seq, date, x, y, seqval );
            break;
        default:
            sts = STS_INVALID_DATA;
            break;
        }
    }

    /*> If either the check or the evaluation gives a coordinate range error,
        then check whether the sequence is defined as having a zero displacement
        outside the coordinate range.  If it is then return without adding the
        displacement.  If it is not, then return an error */

    if( sts == STS_CRD_RANGE_ERR )
    {
        if( seq->zerobeyond ) sts = STS_OK;
        RETURN_STATUS( sts );
    }

    /*> Add the displacement components according to the dimension of the
        sequence (either height only, horizontal only, or 3d) */

    TRACE_LNZDEF(("add_sequence: id = %d, dimension = %d, value = %.4lf %.4lf %.4lf",
                  seq->id, (int)(seq->dimension), seqval[0], seqval[1], seqval[2] ));

    if( seq->dimension == 1 )
    {
        value[2] += seqval[0];
    }
    else
    {
        value[0] += seqval[0];
        value[1] += seqval[1];
        if( seq->dimension == 3 ) value[2] += seqval[2];
    }

    return sts;
}


/*************************************************************************
** Function name: create_def_mod
**//**
**   Function to load the data defining a LINZ deformation model
**
**  \param pdef                The grid object to load
**  \param binsrc              The binary data source to load the
**                             model from
**
**  \return                    The return status
**
**************************************************************************
*/

static StatusType create_def_mod( hDefMod* pdef, hBinSrc binsrc)
{
    hDefMod def;
    int version;
    INT4 indexloc;
    StatusType sts;
    INT2 isgeog=0;
    INT2 nseq=0;
    int idseq;

    (*pdef) = NULL;

    TRACE_LNZDEF(("create_def_mod"));
    version = check_header(binsrc, &indexloc);
    TRACE_LNZDEF(("create_def_mod: version %d  indexloc %ld",version,indexloc));
    if( ! version )
    {
        RETURN_STATUS(STS_INVALID_DATA);
    }
    def = (hDefMod) utlAlloc( sizeof( DefMod ) );
    if( ! def ) RETURN_STATUS(STS_ALLOC_FAILED);

    def->name = NULL;
    def->version = NULL;
    def->crdsyscode = NULL;
    def->description = NULL;
    def->firstseq = NULL;
    def->lastseq = NULL;
    def->binsrc = binsrc;

    TRACE_LNZDEF(("Loading deformation model"));

    sts = STS_OK;
    if( sts == STS_OK ) sts = utlBinSrcLoadString( binsrc, indexloc, &(def->name) );
    if( sts == STS_OK ) sts = utlBinSrcLoadString( binsrc, BINSRC_CONTINUE, &(def->version) );
    if( sts == STS_OK ) sts = utlBinSrcLoadString( binsrc, BINSRC_CONTINUE, &(def->crdsyscode) );
    if( sts == STS_OK ) sts = utlBinSrcLoadString( binsrc, BINSRC_CONTINUE, &(def->description) );

    if( sts == STS_OK ) sts = load_date( binsrc, BINSRC_CONTINUE, &(def->versiondate) );
    if( sts == STS_OK ) sts = load_date( binsrc, BINSRC_CONTINUE, &(def->startdate) );
    if( sts == STS_OK ) sts = load_date( binsrc, BINSRC_CONTINUE, &(def->enddate) );

    if( sts == STS_OK ) sts = utlBinSrcLoad8( binsrc, BINSRC_CONTINUE, 1, &(def->range.ymin) );
    if( sts == STS_OK ) sts = utlBinSrcLoad8( binsrc, BINSRC_CONTINUE, 1, &(def->range.ymax) );
    if( sts == STS_OK ) sts = utlBinSrcLoad8( binsrc, BINSRC_CONTINUE, 1, &(def->range.xmin) );
    if( sts == STS_OK ) sts = utlBinSrcLoad8( binsrc, BINSRC_CONTINUE, 1, &(def->range.xmax) );

    if( sts == STS_OK ) sts = utlBinSrcLoad2( binsrc, BINSRC_CONTINUE, 1, &isgeog );
    def->isgeographical = isgeog ? BLN_TRUE : BLN_FALSE;

    if( sts == STS_OK ) sts = utlBinSrcLoad2( binsrc, BINSRC_CONTINUE, 1, &nseq );

    TRACE_LNZDEF(("Deformation model header loaded: %s",def->name));

    idseq = 0;
    while( sts == STS_OK && nseq-- )
    {
        hDefSeq seq;
        sts = load_sequence( binsrc, &seq );
        if( sts == STS_OK )
        {
            idseq++;
            seq->id = idseq;
            if( ! def->firstseq ) def->firstseq = seq;
            if( def->lastseq ) def->lastseq->nextseq = seq;
            def->lastseq = seq;
        }
    }

    if( sts != STS_OK )
    {
        delete_def_mod( def );
        RETURN_STATUS( sts );
    }


    TRACE_LNZDEF(("Deformation model loaded: %s",def->name));

    *pdef = def;
    return STS_OK;
}


/*************************************************************************
** Function name: calc_def_mod
**//**
**    Calculate the displacement for a deformation model at a specific time
**    and location.
**
**  \param def                 The deformation model
**  \param date                The date (in years)
**  \param x                   The x coordinate
**  \param y                   The y coordinate
**  \param value               Array in which to return the
**                             displacment.
**
**  \return                    The return status
**
**************************************************************************
*/

static StatusType calc_def_mod( hDefMod def, double date,
                                double x, double y, double* value )
{
    hDefSeq seq;
    double calcval[3];
    StatusType sts;

    value[0] = value[1] = value[2] = 0;
    calcval[0] = calcval[1] = calcval[2] = 0;

    /*> If the coordinate are geographical then try to bring the x coordinate
        (longitude) into the range of the model by adding multiples of 360.
        */

    if( def->isgeographical )
    {
        /* Don't attempt to fix stupid values ... */
        if( x > -1000 && x < 1000 )
        {
            while( x < def->range.xmin ) x += 360;
            while( x > def->range.xmax ) x -= 360;
        }
    }

    /*> Check that the coordinates are within range of the model, and if
        not return an error */

    sts = check_range( &(def->range), x, y );
    if( sts != STS_OK ) RETURN_STATUS( sts );

    /*> Check that the date is within range of the model, and if not return an
        error */

    if( date < utlDateAsYear(&(def->startdate)) ||
            date > utlDateAsYear(&(def->enddate)) ) RETURN_STATUS( STS_INVALID_DATA );

    /*> Sum the effects of each deformation sequence using add_sequence */

    TRACE_LNZDEF(("calc_def_mod: Calculating deformation position %.8f %.8f date %.2f",
                  x,y,date));
    for( seq = def->firstseq; seq; seq = seq->nextseq )
    {
        sts = add_sequence( seq, date, x, y, calcval );
        if( sts != STS_OK ) RETURN_STATUS( sts );
    }

    TRACE_LNZDEF(("calc_def_mod: Value calculated as %.4f %.4f %.4f",
                  calcval[0],calcval[1],calcval[2]));



    /*> Store the resulting displacement into the result array */

    value[0] = calcval[0];
    value[1] = calcval[1];
    value[2] = calcval[2];

    return STS_OK;
}


/*************************************************************************
** Function name: utlCreateLinzDef
**//**
**    Create and load a LinzDefModel object from a binary data source.
**    This is used to evaluate deformation at a given time and place.
**    The routine is just a wrapper around ::create_def_mod.
**
**  \param blob                The data source
**  \param pdef                The created model
**
**  \return                    The return status
**
**************************************************************************
*/

StatusType utlCreateLinzDef( hBinSrc blob, hLinzDefModel *pdef )
{
    StatusType sts;
    hDefMod def;

    (*pdef) = NULL;
    sts = create_def_mod( &def, blob );
    if( sts == STS_OK ) (*pdef) = def;

    RETURN_STATUS( sts );
}


/*************************************************************************
** Function name: utlReleaseLinzDef
**//**
**    Release the resources allocated to a LinzDefModel object.
**
**  \param def                 The model to release
**
**  \return                    The return status
**
**************************************************************************
*/

StatusType utlReleaseLinzDef( hLinzDefModel def )
{
    if( def )
    {
        delete_def_mod( (hDefMod) def );
    }
    return STS_OK;
}


/*************************************************************************
** Function name: utlLinzDefCoordSysDef
**//**
**    Get the coordinate system code for the model.
**
**  \param def                 The deformation model
**  \param crdsys              Receives a pointer to a string defining
**                             the coordinate system code.
**
**  \return                    The return status
**
**************************************************************************
*/

StatusType utlLinzDefCoordSysDef( hLinzDefModel def, char ** crdsys )
{
    (*crdsys) = NULL;
    if( ! def ) RETURN_STATUS( STS_INVALID_DATA );
    (*crdsys) = ((hDefMod) def)->crdsyscode;
    return STS_OK;
}


/*************************************************************************
** Function name: utlLinzDefTitle
**//**
**    Returns text description information from a LinzDefModel deformation model
**
**  \param def                 The model
**  \param nTitle              Identifies what is to be returned.
**                               1 is the name
**                               2 is the description
**                               3 is the version number
**  \param title               Returns a pointer to the text
**
**  \return                    The return status
**
**************************************************************************
*/

StatusType utlLinzDefTitle( hLinzDefModel def, int nTitle, char ** title )
{
    (*title) = NULL;
    if( ! def ) RETURN_STATUS( STS_INVALID_DATA );
    if( nTitle < 1 || nTitle > 3 ) RETURN_STATUS( STS_INVALID_DATA );
    switch( nTitle )
    {
    case 1:
        (*title) = ((hDefMod) def)->name;
        break;
    case 2:
        (*title) = ((hDefMod) def)->description;
        break;
    case 3:
        (*title) = ((hDefMod) def)->version;
        break;
    }
    return STS_OK;
}


/*************************************************************************
** Function name: utlCalcLinzDef
**//**
**    Routine to calculate the displacement due to the deformation model
**    at a specific time and place.  This is a wrapper around ::calc_def_mod.
**
**  \param def                 The deformation model
**  \param date                The date at which to evaluate the
**                             model (in years)
**  \param x                   The x coordinate
**  \param y                   The y coordinate
**  \param value               Array to receive the calculated
**                             displacement.
**
**  \return                    The return status
**
**************************************************************************
*/

StatusType utlCalcLinzDef( hLinzDefModel def, double date, double x, double y,
                           double * value)
{

    if( ! def ) RETURN_STATUS( STS_INVALID_DATA );
    return calc_def_mod( (hDefMod) def, date, x, y, value );
}

