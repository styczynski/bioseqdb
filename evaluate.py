import argparse
import pandas as pd
from sklearn.metrics import roc_auc_score


def calculate_auc_roc(output_file, ground_truth_file):
    output = pd.read_csv(output_file, sep='\t')
    ground_truth = pd.read_csv(ground_truth_file, sep='\t')

    # Ensure that fasta_files match
    if not all(output['fasta_file'] == ground_truth['fasta_file']):
        raise ValueError("fasta_files in output and ground truth files do not match.")

    # Get the list of classes from the output file (excluding the 'fasta_file' column)
    classes = output.columns[1:]

    # Calculate AUC-ROC for each class
    auc_scores = []
    for this_class in classes:
        # Ground truth binary labels for the current class
        true_labels = (ground_truth.iloc[:, 1] == this_class).astype(int)

        # Predicted values for the current class
        predicted_scores = output[this_class]

        # Calculate AUC-ROC if there is at least one positive and one negative label
        if true_labels.nunique() > 1:
            auc = roc_auc_score(true_labels, predicted_scores)
            auc_scores.append(auc)
            print(f"AUC-ROC for class {this_class}: {auc:.4f}")
        else:
            print(f"Skipping class {this_class} due to lack of positive/negative samples.")

    # Compute the average AUC-ROC
    average_auc = sum(auc_scores) / len(auc_scores) if auc_scores else 0
    print(f"Average AUC-ROC across all classes: {average_auc:.4f}")
    return average_auc


def main():
    parser = argparse.ArgumentParser(description="Calculate average AUC-ROC from classifier output and ground truth.")
    parser.add_argument("classifier_output", type=str, help="TSV file containing classifier output.")
    parser.add_argument("testing_ground_truth", type=str, help="TSV file containing ground truth classification.")
    args = parser.parse_args()
    calculate_auc_roc(args.classifier_output, args.testing_ground_truth)


if __name__ == "__main__":
    main()
