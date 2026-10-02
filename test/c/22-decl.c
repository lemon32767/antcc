/* EXPECT:
*/

void f(int (*)());
void f(int (*)(int));
void f(int (*)());
int a[];
int a[100];
int a[];

static unsigned local;
extern unsigned local;

typedef struct attrafter { char r; } foo_t;
static const foo_t T = ((foo_t) { 3 });

static const int X = 4;
struct {
   int k : X; /* EXTENSION */
};

foo_t empty[] = {}; /* EXTENSION */

enum e8 : unsigned char { t8 = 255 };
int t2d[3][3] = {[0][1]=1, [1][2]=2, [2][0]=3};

int main() {
   int q = ((struct{_Static_assert(sizeof q>0,"q");} *)0, 1);
   _Static_assert(sizeof(enum e8) == 1, "e8");
   _Static_assert(sizeof t8 == 1, "t8");
}

