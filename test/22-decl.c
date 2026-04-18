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

typedef struct foo { char r; } foo_t;
static const foo_t T = ((foo_t) { 3 });

static const int X = 4;
struct {
   int k : X; /* EXTENSION */
};

foo_t empty[] = {}; /* EXTENSION */

int main() {
}

