package generator.errors;


public class GaussianUniformErrorGenerator extends SubstitutionErrorGenerator
{
	public GaussianUniformErrorGenerator(String allowedCharacters)
	{
		super(allowedCharacters);
		// TODO Auto-generated constructor stub
	}

	@Override
	protected double getSubstitutionProbability(int position, int length)
	{
		// TODO Auto-generated method stub
		return 0;
	}

	@Override
	public CharSequence generateErrors(CharSequence sequence)
	{
		// TODO Auto-generated method stub
		return null;
	}
}
