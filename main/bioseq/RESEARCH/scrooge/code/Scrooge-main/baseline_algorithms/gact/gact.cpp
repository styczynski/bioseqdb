#include <string>
#include <vector>
#include <iostream>

using namespace std;

string gact_tile(string sub_query, string sub_target, int gapo, int gape, int mat, int sub, int TmO, int& consumed_query, int& consumed_target){
    int n = sub_query.size();
    int m = sub_target.size();
    vector<vector<int>> M(n+1);
    vector<vector<int>> I(n+1);
    vector<vector<int>> D(n+1);
    vector<vector<char>> M_source(n+1);
    vector<vector<char>> I_source(n+1);
    vector<vector<char>> D_source(n+1);

    for(int i = n; i >= 0; i--){
        M[i] = vector<int>(m+1, 0);
        I[i] = vector<int>(m+1, 0);
        D[i] = vector<int>(m+1, 0);
        M_source[i] = vector<char>(m+1, ' ');
        I_source[i] = vector<char>(m+1, ' ');
        D_source[i] = vector<char>(m+1, ' ');
    }

    M[n][m] = 0;
    I[n][m] = -10000;
    D[n][m] = -10000;
    for(int j = m-1; j >= 0; j--){
        M[n][j] = (m-j)*gape+gapo;
        I[n][j] = -10000;
        D[n][j] = (m-j)*gape+gapo;
        M_source[n][j] = 'D';
        I_source[n][j] = ' ';
        D_source[n][j] = 'D';
    }

    //for(int i = 0; i <= n; i++){
    //    for(int j = 0; j <= m; j++){
    //        cout << M[i][j] << " ";
    //    }
    //    cout << endl;
    //}
    //cout << endl;

    for(int i = n-1; i >= 0; i--){
        M[i][m] = (n-i)*gape+gapo;
        I[i][m] = (n-i)*gape+gapo;
        D[i][m] = -10000;
        M_source[i][m] = 'I';
        I_source[i][m] = 'I';
        D_source[i][m] = ' ';

    //for(int i = 0; i <= n; i++){
    //    for(int j = 0; j <= m; j++){
    //        cout << M[i][j] << " ";
    //    }
    //    cout << endl;
    //}
    //cout << endl;

        for(int j = m-1; j >= 0; j--){
            I[i][j] = max(I[i+1][j  ], M[i+1][j  ]+gapo) + gape;
            I_source[i][j] = (I[i][j] == I[i+1][j  ] + gape) ? 'I' : 'M';

            D[i][j] = max(D[i  ][j+1], M[i  ][j+1]+gapo) + gape;
            D_source[i][j] = (D[i][j] == D[i  ][j+1] + gape) ? 'D' : 'M';

            int diag = M[i+1][j+1] + (sub_query[i]==sub_target[j] ? mat : sub);
            M[i][j] = max(max(I[i][j], D[i][j]), diag);
            M_source[i][j] = (M[i][j] == diag) ? 'M' : ((M[i][j] == I[i][j]) ? 'I' : 'D');
        }
    }

    //for(int i = 0; i <= n; i++){
    //    for(int j = 0; j <= m; j++){
    //        cout << M[i][j] << " ";
    //    }
    //    cout << endl;
    //}

    int i = 0;
    int j = 0;
    char pos = 'M';

    string cigar;

    char edit = ' ';
    int num_edits = 0;
    while(i < TmO && j < TmO && i < n && j < m){
        char new_edit;
        //bool was_m = (edit==' ') || (edit=='=') || (edit=='X') || (edit=='M');
        //bool was_i = (edit=='I');
        //bool was_d = (edit=='D');

        if(pos=='M'){
            if(M_source[i][j]=='I'){
                //new_edit = 'I';
                pos = 'I';
            }
            else if(M_source[i][j]=='D'){
                //new_edit = 'D';
                pos = 'D';
            }
        }

        if(pos=='I'){
            new_edit = 'I';
            pos = I_source[i][j];
            i++;
        }
        else if(pos=='D'){
            new_edit = 'D';
            pos = I_source[i][j];
            j++;
        }
        else{
            new_edit = (sub_query[i] == sub_target[j]) ? '=' : 'X';
            pos = 'M';
            i++;
            j++;
        }

        if(edit==' '){
            edit = new_edit;
            num_edits = 0;
        }
        else{
            if(edit != new_edit){
                cigar += to_string(num_edits);
                cigar += edit;
                edit = new_edit;
                num_edits = 0;
            }
        }
        num_edits++;
    }
    cigar += to_string(num_edits);
    cigar += edit;

    consumed_query = i;
    consumed_target = j;

    return cigar;
}

string custom_gact(string query, string target, int gapo, int gape, int mat, int sub, int T, int O){
    int n = query.size();
    int m = target.size();
    int i = 0;
    int j = 0;

    string cigar;
    while(i < n && j < m){
        string sub_query = query.substr(i, T);
        string sub_target = target.substr(j, T);
        int consumed_query;
        int consumed_target;
        
        string sub_cigar = gact_tile(sub_query, sub_target, gapo, gape, mat, sub, T-O, consumed_query, consumed_target);
        
        cigar += sub_cigar;
        i += consumed_query;
        j += consumed_target;
    }

    return cigar;
}

//int main(){
//    cout << custom_gact("ACGT", "ACGT", -4, -2, 2, -2, 3, 2) << endl;
//}
