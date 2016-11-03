#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <mpi.h>

/***********************************************************
  Helper functions
***********************************************************/

//For exiting on error condition
void die(int lineNo);

//For trackinng execution
long long wallClockTime();

// For finding max,min
int min(int a, int b);
int max(int a, int b);


/***********************************************************
  Cell related functions, used for storing, managing cells
***********************************************************/

typedef struct CELLS {
    int row;
    int col;
} CELL;

CELL* allocateCellsList( int size );

void printList(CELL* cells, int numCells, int iter, int rotation);


/***********************************************************
  Square/Rectangle matrix related functions, used by both world and pattern
***********************************************************/

char** allocateRectMatrix( int rows, int cols, char defaultValue );

char** allocateSquareMatrix( int size, char defaultValue );

void freeSquareMatrix( char** );

void printSquareMatrix( char**, int size );


/***********************************************************
   World  related functions
***********************************************************/

#define ALIVE 'X'
#define DEAD 'O'

char** readWorldFromFile( char* fname, int* size );

int countNeighbours(char** world, int row, int col);

void evolveWorld(char** curWorld, char** nextWorld, int wRows, int wCols);


/***********************************************************
   Simple circular linked list for match records
***********************************************************/

typedef struct MSTRUCT {
    int iteration, row, col, rotation;
    struct MSTRUCT *next;
} MATCH;


typedef struct {
    int nItem;
    MATCH* tail;
} MATCHLIST;

MATCHLIST* newList();

void deleteList( MATCHLIST*);

void insertEnd(MATCHLIST*, int, int, int, int);

/***********************************************************
   Search related functions
***********************************************************/

//Using the compass direction to indicate the rotation of pattern
#define N 0 //no rotation
#define E 1 //90 degree clockwise
#define S 2 //180 degree clockwise
#define W 3 //90 degree anti-clockwise

char** readPatternFromFile( char* fname, int* size );

void rotate90(char** current, char** rotated, int size);

void searchPatterns(char** world, int startRow, int numRowsToCheck, int wRows, int wCols,
        int iteration, char** patterns[4], int pSize, MATCHLIST* list);

void searchSinglePattern(char** world, int startRow, int numRowsToCheck, int wRows, int wCols,
        int interation, char** pattern, int pSize, int rotation, MATCHLIST* list);

/***********************************************************
   Main function
***********************************************************/


int main( int argc, char** argv)
{
    // Variables
    char **curW, **nextW, **temp, dummy[20];
    char **patterns[4];
    int dir, iterations, iter;
    int size, patternSize;
    long long before, after;

    // Iteration Variables
    int task, rotation, row;

    // MPI Variables
    int rank, numtasks, tag = 0, root = 0;
    MPI_Group orig_group, new_group;
    MPI_Comm new_comm;
    MPI_Request req;
    MPI_Status stat;

    // Check arguments
    if (argc < 4 ){
        fprintf(stderr,
            "Usage: %s <world file> <Iterations> <pattern file>\n", argv[0]);
        exit(1);
    }

    // MPI Init, get rank and number of tasks/processes
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);

    // Create a type for struct CELL, for printing data
    int          nitems = 2;
    int          blocklengths[2] = {1,1};
    MPI_Datatype types[2] = {MPI_INT, MPI_INT};
    MPI_Datatype MPI_CELL_TYPE;
    MPI_Aint     offsets[2];
    offsets[0] = offsetof(CELL, row);
    offsets[1] = offsetof(CELL, col);
    MPI_Type_create_struct(nitems, blocklengths, offsets, types, &MPI_CELL_TYPE);
    MPI_Type_commit(&MPI_CELL_TYPE);

    // Read Files, only in root task.
    if (rank == root) {
        curW = readWorldFromFile(argv[1], &size);
        nextW = allocateSquareMatrix(size+2, DEAD);
        printf("World Size = %d\n", size);

        iterations = atoi(argv[2]);
        printf("Iterations = %d\n", iterations);

        patterns[N] = readPatternFromFile(argv[3], &patternSize);
        for (dir = E; dir <= W; dir++){
            patterns[dir] = allocateSquareMatrix(patternSize, DEAD);
            rotate90(patterns[dir-1], patterns[dir], patternSize);
        }
        printf("Pattern size = %d\n", patternSize);
    }

    //Start timer
    if (rank == root)
        before = wallClockTime();

    // -------------------------------------------------------------------------
    // Data Distribution
    // -------------------------------------------------------------------------
    // Broadcast world size + pattern size + #iterations
    MPI_Bcast(&size, 1, MPI_INT, root, MPI_COMM_WORLD);
    MPI_Bcast(&patternSize, 1, MPI_INT, root, MPI_COMM_WORLD);
    MPI_Bcast(&iterations, 1, MPI_INT, root, MPI_COMM_WORLD);

    // Number of rows that may contain the top-left cell of the glider
    int rowsGlider = max(1, size - 4);
    // Max number of rows a task need to handle (check for glider with top-left cells)
    int rowsPerTask = ceil((double)rowsGlider / numtasks);
    // First row to handle
    int startRow = rank * rowsPerTask + 1;
    // number of rows the current task need to check for maching patterns
    int numRows = min(rowsPerTask, rowsGlider - startRow + 1);
    // number extra rows that the task need for the calculations.
    int extraRows = patternSize + (startRow+numRows+patternSize-1 > size);
    // total rows that the task need to store
    int totalRows = numRows + extraRows;

    if (numRows < 1) {
        // Case redundant tasks, no more rows to handle/check for patterns -> ignore
        MPI_Finalize();
        return 0;
    }

    // Update correct number of running tasks
    numtasks = ceil((double)rowsGlider / rowsPerTask);

    // Data Allocation for non-root tasks.
    if (rank != root) {
        curW = allocateRectMatrix(totalRows, size+2, DEAD);
        nextW = allocateRectMatrix(totalRows, size+2, DEAD);
        for (dir = N; dir <= W; dir++)
            patterns[dir] = allocateSquareMatrix(patternSize, DEAD);
    }

    // Broascast Patterns
    for (dir = N; dir <= W; dir++)
        MPI_Bcast(&patterns[dir][0][0], patternSize*patternSize, MPI_CHAR, root, MPI_COMM_WORLD);

    // Send corresponding rows to tasks/processes
    if (rank == root) {
        for (task = 1, row = rowsPerTask + 1; task < numtasks; task++, row += rowsPerTask) {
            // Sending to task with rank=task
            int mainRowsToSend = min(rowsPerTask, rowsGlider - row + 1);
            int extraRowsToSend = patternSize + (row+mainRowsToSend+patternSize-1 > size);
            int totalRowsToSend = mainRowsToSend + extraRowsToSend;
            MPI_Isend(&curW[row - 1][0], totalRowsToSend * (size + 2), MPI_CHAR, task, tag, MPI_COMM_WORLD, &req);
        }
    } else {
        // Blocking receive data from root
        MPI_Recv(&curW[0][0], totalRows * (size + 2), MPI_CHAR, root, tag, MPI_COMM_WORLD, &stat);
    }

    // -------------------------------------------------------------------------
    // Actual work start, searching for patterns
    // -------------------------------------------------------------------------
    // List for results
    MATCHLIST *list;
    list= newList();

    for (iter = 0; iter < iterations; iter++) {
        // Search for Patterns
        searchPatterns( curW, startRow, numRows, totalRows, size, iter, patterns, patternSize, list);

        // Generate next generation
        evolveWorld( curW, nextW, totalRows, size );
        temp = curW;
        curW = nextW;
        nextW = temp;

        // Send additional rows to neighbors (rows that neighbors cannot calculate by themselves)
        if (rank != numtasks-1)
            MPI_Isend(&curW[numRows][1], size, MPI_CHAR, rank+1, iter, MPI_COMM_WORLD, &req);
        if (rank != 0)
            MPI_Isend(&curW[patternSize-1][1], size, MPI_CHAR, rank-1, iterations+iter, MPI_COMM_WORLD, &req);

        // Blocking receive rows from the neighbors, that I cannot calculate by myself
        if (rank != 0)
            MPI_Recv(&curW[0][1], size, MPI_CHAR, rank-1, iter, MPI_COMM_WORLD, &stat);
        if (rank != numtasks-1)
            MPI_Recv(&curW[totalRows-1][1], size, MPI_CHAR, rank+1, iterations+iter, MPI_COMM_WORLD, &stat);
    }

    // -------------------------------------------------------------------------
    // Print the results in order
    // -------------------------------------------------------------------------

    // Send the sizes of results from all ranks to the root and print
    int totalSize, listSize = list->nItem;
    MPI_Reduce(&listSize, &totalSize, 1, MPI_INT, MPI_SUM, root, MPI_COMM_WORLD);
    if (rank == root)
        printf("List size = %d\n", totalSize);

    // Iterator in each rank
    MATCH* cur;
    cur = list->tail->next;
    int count = 1;

    // Cells to print
    CELL *cellsToPrint;
    cellsToPrint = allocateCellsList(size * rowsPerTask);
    int numCells;

    // FOR EACH (iteration, rotation)
    for (iter = 0; iter < iterations; iter++) {
        for (rotation = 0; rotation < 4; rotation++) {
            // Get list of results corresponding to (iteration, rotation)
            numCells = 0;
            while (count <= listSize && cur->iteration == iter && cur->rotation == rotation) {
                cellsToPrint[numCells].row = cur->row;
                cellsToPrint[numCells].col = cur->col;
                numCells ++; count ++;
                cur = cur->next;
            }

            if (rank == root) {
                // Only printing at root
                printList(cellsToPrint, numCells, iter, rotation);
                for (task = 1; task < numtasks; task ++) {
                    MPI_Probe(task, tag, MPI_COMM_WORLD, &stat);
                    MPI_Get_count(&stat, MPI_CELL_TYPE, &numCells);
                    MPI_Recv(&cellsToPrint[0], numCells, MPI_CELL_TYPE, task, tag, MPI_COMM_WORLD, &stat);
                    printList(cellsToPrint, numCells, iter, rotation);
                }
            } else {
                // Non-root tasks: send printing data to root
                MPI_Send(&cellsToPrint[0], numCells, MPI_CELL_TYPE, root, tag, MPI_COMM_WORLD);
            }
        }
    }

    //Stop timer
    if (rank == root) {
        after = wallClockTime();
        printf("Parallel SETL took %1.2f seconds\n", ((float)(after - before))/1000000000);
    }

    //Clean up
    deleteList( list );
    freeSquareMatrix( curW );
    freeSquareMatrix( nextW );
    for (rotation = 0; rotation < 4; rotation++)
        freeSquareMatrix( patterns[rotation] );
    free(cellsToPrint);

    MPI_Finalize();
    return 0;
}

/***********************************************************
  Helper functions
***********************************************************/


void die(int lineNo)
{
    fprintf(stderr, "Error at line %d. Exiting\n", lineNo);
    exit(1);
}

long long wallClockTime( )
{
#ifdef __linux__
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);
    return (long long)(tp.tv_nsec + (long long)tp.tv_sec * 1000000000ll);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)(tv.tv_usec * 1000 + (long long)tv.tv_sec * 1000000000ll);
#endif
}

int min(int a, int b) {
    return a < b ? a : b;
}

int max(int a, int b) {
    return a > b ? a : b;
}

/***********************************************************
  Square/Rectangle matrix related functions, used by both world and pattern
***********************************************************/
char** allocateRectMatrix( int rows, int cols, char defaultValue ) {
    char* contiguous;
    char** matrix;
    int i;

    //Using a least compiler version dependent approach here
    //C99, C11 have a nicer syntax.
    contiguous = (char*) malloc(sizeof(char) * rows * cols);
    if (contiguous == NULL)
        die(__LINE__);

    memset(contiguous, defaultValue, rows * cols );

    //Point the row array to the right place
    matrix = (char**) malloc(sizeof(char*) * rows );
    if (matrix == NULL)
        die(__LINE__);

    matrix[0] = contiguous;
    for (i = 1; i < rows; i++){
        matrix[i] = &contiguous[i*cols];
    }

    return matrix;
}

char** allocateSquareMatrix( int size, char defaultValue )
{
    char* contiguous;
    char** matrix;
    int i;

    //Using a least compiler version dependent approach here
    //C99, C11 have a nicer syntax.
    contiguous = (char*) malloc(sizeof(char) * size * size);
    if (contiguous == NULL)
        die(__LINE__);


    memset(contiguous, defaultValue, size * size );

    //Point the row array to the right place
    matrix = (char**) malloc(sizeof(char*) * size );
    if (matrix == NULL)
        die(__LINE__);

    matrix[0] = contiguous;
    for (i = 1; i < size; i++){
        matrix[i] = &contiguous[i*size];
    }

    return matrix;
}

void printSquareMatrix( char** matrix, int size )
{
    int i,j;

    for (i = 0; i < size; i++){
        for (j = 0; j < size; j++){
            printf("%c", matrix[i][j]);
        }
        printf("\n");
    }
    printf("\n");
}

void freeSquareMatrix( char** matrix )
{
    if (matrix == NULL) return;

    free( matrix[0] );
}

/***********************************************************
   World  related functions
***********************************************************/

char** readWorldFromFile( char* fname, int* sizePtr )
{
    FILE* inf;

    char temp, **world;
    int i, j;
    int size;

    inf = fopen(fname,"r");
    if (inf == NULL)
        die(__LINE__);


    fscanf(inf, "%d", &size);
    fscanf(inf, "%c", &temp);

    //Using the "halo" approach
    // allocated additional top + bottom rows
    // and leftmost and rightmost rows to form a boundary
    // to simplify computation of cell along edges
    world = allocateSquareMatrix( size + 2, DEAD );

    for (i = 1; i <= size; i++){
        for (j = 1; j <= size; j++){
            fscanf(inf, "%c", &world[i][j]);
        }
        fscanf(inf, "%c", &temp);
    }

    *sizePtr = size;    //return size
    return world;

}

int countNeighbours(char** world, int row, int col)
//Assume 1 <= row, col <= size, no check
{
    int i, j, count;

    //discount the center
    count = -(world[row][col] == ALIVE);

    for(i = row-1; i <= row+1; i++){
        for(j = col-1; j <= col+1; j++){
            count += (world[i][j] == ALIVE );
        }
    }

    return count;

}

void evolveWorld(char** curWorld, char** nextWorld, int wRows, int wCols)
{
    int i, j, liveNeighbours;

    for (i = 1; i < wRows-1; i++){
        for (j = 1; j <= wCols; j++){
            liveNeighbours = countNeighbours(curWorld, i, j);
            nextWorld[i][j] = DEAD;

            //Only take care of alive cases
            if (curWorld[i][j] == ALIVE) {

                if (liveNeighbours == 2 || liveNeighbours == 3)
                    nextWorld[i][j] = ALIVE;

            } else if (liveNeighbours == 3)
                    nextWorld[i][j] = ALIVE;
        }
    }
}

/***********************************************************
   Search related functions
***********************************************************/

char** readPatternFromFile( char* fname, int* sizePtr )
{
    FILE* inf;

    char temp, **pattern;
    int i, j;
    int size;

    inf = fopen(fname,"r");
    if (inf == NULL)
        die(__LINE__);


    fscanf(inf, "%d", &size);
    fscanf(inf, "%c", &temp);

    pattern = allocateSquareMatrix( size, DEAD );

    for (i = 0; i < size; i++){
        for (j = 0; j < size; j++){
            fscanf(inf, "%c", &pattern[i][j]);
        }
        fscanf(inf, "%c", &temp);
    }

    *sizePtr = size;    //return size
    return pattern;
}


void rotate90(char** current, char** rotated, int size)
{
    int i, j;

    for (i = 0; i < size; i++){
        for (j = 0; j < size; j++){
            rotated[j][size-i-1] = current[i][j];
        }
    }
}

void searchPatterns(char** world, int startRow, int numRowsToCheck, int wRows, int wCols,
        int iteration, char** patterns[4], int pSize, MATCHLIST* list)
{
    int dir;

    for (dir = N; dir <= W; dir++){
        searchSinglePattern(world, startRow, numRowsToCheck, wRows, wCols, iteration,
                patterns[dir], pSize, dir, list);
    }

}

void searchSinglePattern(char** world, int startRow, int numRowsToCheck, int wRows, int wCols,
        int iteration, char** pattern, int pSize, int rotation, MATCHLIST* list)
{
    int wRow, wCol, pRow, pCol, match;

    for (wRow = 1; wRow <= numRowsToCheck; wRow++) {
        for (wCol = 1; wCol <= (wCols-pSize+1); wCol++) {
            match = 1;

            for (pRow = 0; match && pRow < pSize; pRow++) {
                for (pCol = 0; pCol < pSize; pCol++) {
                    if (world[wRow+pRow][wCol+pCol] != pattern[pRow][pCol]) {
                        match = 0;
                        break;
                    }
                }
            }
            if (match)
                insertEnd(list, iteration, startRow+wRow-2, wCol-1, rotation);
        }
    }
}

/***********************************************************
   Simple circular linked list for match records
***********************************************************/

MATCHLIST* newList()
{
    MATCHLIST* list;

    list = (MATCHLIST*) malloc(sizeof(MATCHLIST));
    if (list == NULL)
        die(__LINE__);

    list->nItem = 0;
    list->tail = NULL;

    return list;
}

void deleteList( MATCHLIST* list)
{
    MATCH *cur, *next;
    int i;
    //delete items first

    if (list->nItem != 0 ){
        cur = list->tail->next;
        next = cur->next;
        for( i = 0; i < list->nItem; i++, cur = next, next = next->next ) {
            free(cur);
        }

    }
    free( list );
}

void insertEnd(MATCHLIST* list,
        int iteration, int row, int col, int rotation)
{
    MATCH* newItem;

    newItem = (MATCH*) malloc(sizeof(MATCH));
    if (newItem == NULL)
        die(__LINE__);

    newItem->iteration = iteration;
    newItem->row = row;
    newItem->col = col;
    newItem->rotation = rotation;

    if (list->nItem == 0){
        newItem->next = newItem;
        list->tail = newItem;
    } else {
        newItem->next = list->tail->next;
        list->tail->next = newItem;
        list->tail = newItem;
    }

    (list->nItem)++;

}


/***********************************************************
   Cells related functions
***********************************************************/

CELL* allocateCellsList( int size) {
    CELL* contiguous;

    //Using a least compiler version dependent approach here
    //C99, C11 have a nicer syntax.
    contiguous = (CELL*) malloc(sizeof(CELL) * size);
    if (contiguous == NULL)
        die(__LINE__);

    return contiguous;
}

void printList(CELL* cells, int numCells, int iter, int rotation) {
    int i;
    for (i = 0; i < numCells; i ++)
        printf("%d:%d:%d:%d\n", iter, cells[i].row, cells[i].col, rotation);
}
